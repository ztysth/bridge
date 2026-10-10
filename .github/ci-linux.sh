#!/usr/bin/env bash
set -euo pipefail
# Ubuntu 24.04 SDK, shared by native and emulated builds.
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update
sudo apt-get install -y g++-13 cmake ninja-build pkg-config libssl-dev catch2 libutf8proc-dev=2.9.0-1build1 \
  qt6-base-dev=6.4.2+dfsg-21.1build5 qt6-declarative-dev=6.4.2+dfsg-4build3 \
  qt6-declarative-dev-tools qt6-qmltooling-plugins \
  qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-dialogs qml6-module-qtquick-templates \
  qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript qml6-module-qt-labs-folderlistmodel \
  libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-dev dpkg-dev file
cmake --preset release-system -B "$BRIDGE_CI_BUILD" \
  -DCMAKE_CXX_COMPILER=g++-13 -DBRIDGE_LINUX_SYSTEM_PACKAGE=ON
cmake --build "$BRIDGE_CI_BUILD" --parallel 2
ctest --test-dir "$BRIDGE_CI_BUILD" --output-on-failure
python3 -m unittest discover -s tests/tooling
bridge_package_arguments=()
if [[ "${BRIDGE_EMULATED:-0}" == 1 ]]; then
  bridge_package_arguments+=(--emulated)
fi
python3 .github/package.py package --build "$BRIDGE_CI_BUILD" --output "$BRIDGE_CI_DIST" \
  --platform linux --arch "$BRIDGE_CI_ARCH" "${bridge_package_arguments[@]}"
