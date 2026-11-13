#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -f /.dockerenv ] && command -v docker >/dev/null 2>&1; then
    if ! docker image inspect dirlocker-builder:20.04 >/dev/null 2>&1; then
        docker build --network=host -f "${ROOT_DIR}/Dockerfile.builder" -t dirlocker-builder:20.04 "${ROOT_DIR}"
    fi
    exec docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "${ROOT_DIR}:/workspace" -w /workspace \
        dirlocker-builder:20.04 bash /workspace/scripts/build_all.sh
fi

echo "=== [1/4] Building Linux (Ubuntu 20.04 / Kylin V10 SP1 x64) ==="
cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build-linux" -DCMAKE_BUILD_TYPE=Release
cmake --build "${ROOT_DIR}/build-linux" -j"$(nproc)"

echo "=== [2/4] Running Core Unit Tests ==="
(cd "${ROOT_DIR}/build-linux" && ctest --output-on-failure)
"${ROOT_DIR}/build-linux/test_core"

echo "=== [3/4] Packaging Kylin V10 SP1 .deb & Manual Installer ==="
KYLIN_DIST="${ROOT_DIR}/dist/kylin-ubuntu20.04"
rm -rf "${KYLIN_DIST}"
mkdir -p "${KYLIN_DIST}/installer"

bash "${ROOT_DIR}/scripts/package_linux_deb.sh" \
    "${ROOT_DIR}/build-linux" \
    "${KYLIN_DIST}/dirlocker_2.2.2_amd64.deb"

cp "${ROOT_DIR}/build-linux/dirlockerd" \
   "${ROOT_DIR}/build-linux/dirlocker" \
   "${ROOT_DIR}/build-linux/libdirlocker-peony-plugin.so" \
   "${ROOT_DIR}/src/linux/packaging/dirlockerd.service" \
   "${ROOT_DIR}/src/linux/packaging/dirlocker.desktop" \
   "${ROOT_DIR}/src/linux/packaging/dirlocker-opener.desktop" \
   "${ROOT_DIR}/src/linux/packaging/dirlocker-autostart.desktop" \
   "${ROOT_DIR}/src/linux/packaging/dirlocker-mime.xml" \
   "${ROOT_DIR}/src/linux/packaging/install.sh" \
   "${ROOT_DIR}/src/linux/packaging/uninstall.sh" \
   "${KYLIN_DIST}/installer/"
cp -r "${ROOT_DIR}/src/linux/packaging/icons" "${KYLIN_DIST}/installer/"
chmod 755 "${KYLIN_DIST}/installer/install.sh" "${KYLIN_DIST}/installer/uninstall.sh"

echo "=== [4/4] Cross-compiling Windows 10/11 x64 ==="
cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build-win64" \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${ROOT_DIR}/build-win64" -j"$(nproc)"

WIN_DIST="${ROOT_DIR}/dist/windows"
rm -rf "${WIN_DIST}"
mkdir -p "${WIN_DIST}"

echo "=== [4b] Building Windows NSIS GUI Installer ==="
makensis -DAPP_VERSION=2.2.2 \
    -DSRC_WIN_DIR="${ROOT_DIR}/src/windows" \
    -DBIN_DIR="${ROOT_DIR}/build-win64" \
    -DOUT_FILE="${WIN_DIST}/DirLocker_Setup_v2.2.2.exe" \
    "${ROOT_DIR}/src/windows/installer/dirlocker.nsi"

echo "=== Build Complete ==="
ls -lh "${KYLIN_DIST}/dirlocker_2.2.2_amd64.deb" "${WIN_DIST}/DirLocker_Setup_v2.2.2.exe"
