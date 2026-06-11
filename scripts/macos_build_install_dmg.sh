#!/usr/bin/env bash
#
# Build Sunshine.dmg and install it to /Applications.
# Make sure to stop the previous version first.
#
# Recommended command-line args:
# --skip-notarize --skip-tests --debug --num-processors=10

set -euo pipefail

readonly DMG_PATH="build/cpack_artifacts/Sunshine.dmg"
readonly MOUNT_POINT="/Volumes/Sunshine"
readonly APP_NAME="Sunshine.app"
readonly INSTALL_DIR="/Applications"

mounted=false

cleanup() {
  if [[ "$mounted" == true ]] && mountpoint -q "$MOUNT_POINT"; then
    hdiutil detach "$MOUNT_POINT" >/dev/null
  fi
}

trap cleanup EXIT

scripts/macos_build.sh "$@"

# Remove a stale mount left by an earlier failed run.
if mountpoint -q "$MOUNT_POINT"; then
  hdiutil detach "$MOUNT_POINT" >/dev/null
fi

# `yes` normally exits with SIGPIPE after hdiutil stops reading. Temporarily
# disable pipefail so the pipeline status reflects hdiutil rather than yes.
set +o pipefail
yes | PAGER=cat hdiutil attach -quiet -nobrowse -mountpoint "$MOUNT_POINT" "$DMG_PATH"
attach_status=${PIPESTATUS[1]}
set -o pipefail

if (( attach_status != 0 )); then
  echo "Failed to mount $DMG_PATH" >&2
  exit "$attach_status"
fi

mounted=true

rm -rf "$INSTALL_DIR/$APP_NAME"
ditto "$MOUNT_POINT/$APP_NAME" "$INSTALL_DIR/$APP_NAME"

hdiutil detach "$MOUNT_POINT" >/dev/null
mounted=false

echo "Installed: "
ls -l "$INSTALL_DIR/$APP_NAME"
