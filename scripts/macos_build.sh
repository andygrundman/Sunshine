#!/usr/bin/env bash

# Install deps via brew first

BUILD_SYSTEM="Unix Makefiles" # or Xcode (needs work)
BUILD_TYPE=Debug

pushd $(git rev-parse --show-toplevel)

BRANCH=$(git rev-parse --abbrev-ref HEAD)
BUILD_VERSION=
COMMIT=$(git rev-parse --short HEAD)

mkdir -p build && \
    cmake -S . -B build -G "$BUILD_SYSTEM" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_WERROR=ON \
    -DHOMEBREW_ALLOW_FETCHCONTENT=ON \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/Cellar/openssl@3/3.4.0 \
    -DSUNSHINE_ASSETS_DIR=sunshine/assets \
    -DSUNSHINE_BUILD_HOMEBREW=ON \
    -DSUNSHINE_ENABLE_TRAY=OFF \
    -DSUNSHINE_PUBLISHER_NAME='LizardByte' \
    -DSUNSHINE_PUBLISHER_WEBSITE='https://app.lizardbyte.dev' \
    -DSUNSHINE_PUBLISHER_ISSUE_URL='https://app.lizardbyte.dev/support' \
    -DBUILD_DOCS=OFF \
    -DBOOST_USE_STATIC=OFF

# For Unix Makefiles: cd build && make -j 8

# For Xcode: open build/Sunshine.xcodeproj
