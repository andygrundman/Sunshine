#!/usr/bin/env bash
set -euo pipefail

# Default value for arguments
num_processors=$(sysctl -n hw.ncpu)
publisher_name="LizardByte"
publisher_website="https://app.lizardbyte.dev"
publisher_issue_url="https://app.lizardbyte.dev/support"
step="all"
build_docs="ON"
build_tests="ON"
build_type="Release"
sign_app="true"
notarize="OFF"

# environment variables
#BUILD_VERSION=""
BRANCH=$(git rev-parse --abbrev-ref HEAD)
COMMIT=$(git rev-parse --short HEAD)

# v2026.206.151412
#export BUILD_VERSION
export BRANCH
export COMMIT

# boost could be included here but cmake will build the right version we need
required_formulas=(
  "cmake"
  "doxygen"
  "graphviz"
  "node"
  "pkgconf"
  "icu4c@78"
  "miniupnpc"
  "openssl@3"
  "opus"
  "llvm"
)

# brew libraries that we will link with, these need to be fat binaries
to_fatten=(
  "boost"
  "miniupnpc"
  "openssl@3"
  "opus"
)

function _usage() {
  local exit_code=$1

  cat <<EOF
This script builds a macOS .app bundle packaged inside a .dmg.

If the environment variable CODESIGN_IDENTITY is set, the app will be signed.
This must be a "Developer ID" identity.

For others to be able to open the .dmg, it must be notarized. Create a keychain profile named
"notarytool-password" based on the instructions at
https://developer.apple.com/documentation/security/customizing-the-notarization-workflow?language=objc

Usage:
  $0 [options]

Options:
  -h, --help               Display this help message.
  --num-processors         The number of processors to use for compilation. Default: ${num_processors}.
  --publisher-name         The name of the publisher (not developer) of the application.
  --publisher-website      The URL of the publisher's website.
  --publisher-issue-url    The URL of the publisher's support site or issue tracker.
                           If you provide a modified version of Sunshine, we kindly request that you use your own url.
  --step=STEP              Which step(s) to run: deps, cmake, build, dmg, or all (default: all)
  --debug                  Build in debug mode.
  --skip-docs              Don't build docs.
  --skip-tests             Don't build the test suite.
  --skip-codesign          Don't sign the bundle.
  --notarize               Notarize the final .dmg with Apple's GateKeeper so it can be installed without warnings.

Steps:
  deps                     Install dependencies only
  cmake                    Run cmake configure only
  build                    Build the project only
  dmg                      Create a DMG package
  all                      Run all steps (default)
EOF

  exit "$exit_code"
}

create_universal() {
  local pkg="${1:?package name required}"
  local out_local="${build_dir}/local-universal"
  local tmp_x86="${build_dir}/local-universal/${pkg}-x86_64"

  echo
  echo "==> Making universal binary for: ${pkg}"

  # Fetch both bottles, since the arm64 one has probably been removed
  brew fetch --force "${pkg}"
  brew fetch --force --bottle-tag=x86_64_sonoma "${pkg}"

  local arm_tgz x86_tgz
  arm_tgz="$(brew --cache "${pkg}")"
  x86_tgz="$(brew --cache --bottle-tag=x86_64_sonoma "${pkg}")"

  [[ -f "${arm_tgz}" ]] || { echo "ERROR: missing arm64 bottle: ${arm_tgz}" >&2; return 1; }
  [[ -f "${x86_tgz}" ]] || { echo "ERROR: missing x86_64 bottle: ${x86_tgz}" >&2; return 1; }

  # Extract arm64 bottle
  mkdir -p "${out_local}"
  /usr/bin/tar -xzf "${arm_tgz}" -C "${out_local}" --strip-components=2

  # Extract x86 bottle, only the libraries. Uses explicit system tar for --include.
  rm -rf "${tmp_x86}"
  mkdir -p "${tmp_x86}"
  /usr/bin/tar -xzf "${x86_tgz}" -C "${tmp_x86}" --strip-components=2 --include='*.dylib' --include='*.a'

  # Fatten all .a and .dylib files found in the x86 tree
  local x86_file rel arm_file
  while IFS= read -r -d '' x86_file; do
    # Path relative to tmp_x86 (e.g. "lib/foo/libbar.a")
    rel="${x86_file#${tmp_x86}/}"
    arm_file="${out_local}/${rel}"

    if [[ ! -f "${arm_file}" ]]; then
      echo "warning: missing arm64 counterpart for ${rel}"
      continue
    fi

    echo "lipo: ${rel}"
    lipo -create -output "${arm_file}" "${arm_file}" "${x86_file}"

    if [[ "$arm_file" == *.dylib ]]; then
      local token='@@HOMEBREW_PREFIX@@'
      install_name_tool -change "${token}" "${out_local}" "${arm_file}"
    fi
  done < <(find "${tmp_x86}" -type f \( -name "*.a" -o -name "*.dylib" \) -print0)
}

function run_step_deps() {
  echo "Running step: Install dependencies"
  brew update
  brew install "${required_formulas[@]}"

  for pkg in "${to_fatten[@]}"; do
      create_universal "${pkg}"
  done

  return 0
}

function run_step_cmake() {
  echo "Running step: CMake configure"

  # point to our universal libs
  FFMPEG_ROOT="/Users/andy/Downloads/universal/ffmpeg"
  MACOS_UNIVERSAL_PREFIX="${build_dir}/local-universal"

  # prepare CMAKE args
  cmake_args=(
    "-B=build"
    "-S=."
    "-DCMAKE_BUILD_TYPE=${build_type}"
    "-DCMAKE_PREFIX_PATH=${MACOS_UNIVERSAL_PREFIX}"
    "-DBoost_VERBOSE=ON"
    "-DBUILD_WERROR=ON"
    "-DCMAKE_POLICY_DEFAULT_CMP0167=OLD" # keep FindBoost module available when needed
    "-DFFMPEG_PREPARED_BINARIES=${FFMPEG_ROOT}"
    "-DMACOS_UNIVERSAL_PREFIX=${MACOS_UNIVERSAL_PREFIX}"
    "-DOPENSSL_ROOT_DIR=${MACOS_UNIVERSAL_PREFIX}"
    "-DSUNSHINE_BOOST_LIBRARY_DIR=${MACOS_UNIVERSAL_PREFIX}/lib"
    "-DSUNSHINE_BUILD_HOMEBREW=OFF"
    "-DSUNSHINE_ENABLE_TRAY=ON"
    "-DBUILD_DOCS=${build_docs}"
    "-DBUILD_TESTS=${build_tests}"
    "-DBOOST_USE_STATIC=OFF"
    "-DSUNSHINE_BUNDLE_IDENTIFIER=dev.lizardbyte.sunshine"
    "-DNOTARIZE=${notarize}"
  )

  if [[ -n "${sign_app}" ]]; then
    if [[ -n "${CODESIGN_IDENTITY:-}" ]]; then
      cmake_args+=("-DCODESIGN_IDENTITY='${CODESIGN_IDENTITY}'")
    else
      echo "Please set the CODESIGN_IDENTITY environment variable or use --skip-codesign"
      exit 1
    fi
  fi

  # Publisher metadata
  if [[ -n "$publisher_name" ]]; then
    cmake_args+=("-DSUNSHINE_PUBLISHER_NAME='${publisher_name}'")
  fi
  if [[ -n "$publisher_website" ]]; then
    cmake_args+=("-DSUNSHINE_PUBLISHER_WEBSITE='${publisher_website}'")
  fi
  if [[ -n "$publisher_issue_url" ]]; then
    cmake_args+=("-DSUNSHINE_PUBLISHER_ISSUE_URL='${publisher_issue_url}'")
  fi

  # Cmake stuff here
  mkdir -p "build"
  echo "cmake args:"
  echo "${cmake_args[@]}"
  cmake "${cmake_args[@]}"
  return 0
}

function run_step_build() {
  echo "Running step: Build"
  cmake --build "${build_dir}" -j "${num_processors}"
  return 0
}

function run_step_dmg() {
  echo "Running step: Creating DMG package"
  cpack -G DragNDrop --config "${build_dir}/CPackConfig.cmake" --verbose

  if [[ "$notarize" == "ON" ]]; then
    xcrun notarytool submit "${build_dir}/cpack_artifacts/Sunshine.dmg" --keychain-profile "notarytool-password" --wait
    xcrun stapler staple -v "${build_dir}/cpack_artifacts/Sunshine.dmg"
  fi
  return 0
}

function run_install() {
  case "$step" in
    deps)
      run_step_deps
      ;;
    cmake)
      run_step_cmake
      ;;
    build)
      run_step_build
      ;;
    dmg)
      run_step_dmg
      ;;
    all)
      run_step_deps
      run_step_cmake
      run_step_build
      run_step_dmg
      ;;
    *)
      echo "Invalid step: $step"
      echo "Valid steps are: deps, cmake, build, dmg, all"
      exit 1
      ;;
  esac
  return 0
}

# Parse named arguments
while getopts ":h-:" opt; do
  case ${opt} in
    h ) _usage 0 ;;
    - )
      case "${OPTARG}" in
        help) _usage 0 ;;
        num-processors=*)
          num_processors="${OPTARG#*=}"
          ;;
        publisher-name=*)
          publisher_name="${OPTARG#*=}"
          ;;
        publisher-website=*)
          publisher_website="${OPTARG#*=}"
          ;;
        publisher-issue-url=*)
          publisher_issue_url="${OPTARG#*=}"
          ;;
        step=*)
          step="${OPTARG#*=}"
          ;;
        debug)
          build_type="Debug"
          ;;
        skip-docs)
          build_docs="OFF"
          ;;
        skip-tests)
          build_tests="OFF"
          ;;
        skip-codesign)
         sign_app=""
          ;;
        notarize)
          notarize="ON"
          ;;
        *)
          echo "Invalid option: --${OPTARG}" 1>&2
          _usage 1
          ;;
      esac
      ;;
    \? )
      echo "Invalid option: -${OPTARG}" 1>&2
      _usage 1
      ;;
  esac
done
shift $((OPTIND -1))

# get directory of this script
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
build_dir="$script_dir/../build"
echo "Script Directory: $script_dir"
echo "Build Directory: $build_dir"
mkdir -p "$build_dir"

run_install
