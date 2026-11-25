#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_VERSION="3.0.0"

echo "================================================================"
echo "  目录密匣 (DirLocker) v${APP_VERSION} — Wails Fluent Mica 双平台构建"
echo "================================================================"

# Ensure Wails app icon & Windows icon are synced from Fluent Design 2 assets
mkdir -p "${ROOT_DIR}/build/windows"
cp "${ROOT_DIR}/src/linux/packaging/icons/apps/256x256/dirlocker.png" "${ROOT_DIR}/build/appicon.png"
cp "${ROOT_DIR}/src/windows/app.ico" "${ROOT_DIR}/build/windows/icon.ico"

# Step 1: Build Linux C++ root daemon (dirlockerd), Peony extension, and run C++ core unit tests
echo "=== [1/4] Building Linux C++ daemon, Peony plugin & running C++ tests ==="
if ! docker image inspect dirlocker-builder:20.04 >/dev/null 2>&1; then
    docker build --network=host -f "${ROOT_DIR}/Dockerfile.builder" -t dirlocker-builder:20.04 "${ROOT_DIR}"
fi
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "${ROOT_DIR}:/workspace" -w /workspace \
    dirlocker-builder:20.04 bash -c '
        set -euo pipefail
        cmake -S /workspace -B /workspace/build-linux -DCMAKE_BUILD_TYPE=Release
        cmake --build /workspace/build-linux --target dirlockerd dirlocker-peony-plugin test_core -j"$(nproc)"
        cd /workspace/build-linux && ctest --output-on-failure && ./test_core
    '

# Step 2: Run Go unit tests + Build Wails Linux & Windows GUIs + Package .deb & NSIS .exe
echo "=== [2/4] Running Go tests & building Wails Fluent Mica GUIs (Linux + Windows) ==="
docker run --rm \
    -e HOST_UID="$(id -u)" \
    -e HOST_GID="$(id -g)" \
    -v docpurger-gomod-cache:/root/go/pkg/mod \
    -v "${ROOT_DIR}:/workspace" -w /workspace \
    docpurger-builder:20.04 bash -lc "
        set -euo pipefail
        trap 'chown -R \"\$HOST_UID:\$HOST_GID\" /workspace/build /workspace/build-linux /workspace/build-win64 /workspace/dist /workspace/frontend/dist 2>/dev/null || true' EXIT

        echo '--- Go unit tests & vet ---'
        go test -v ./internal/...
        go vet ./internal/...

        echo '--- Frontend TypeScript + Vite build ---'
        cd /workspace/frontend
        if [ ! -d node_modules ]; then
            npm ci
        fi
        npm run build
        cd /workspace

        echo '--- Wails Linux (Ubuntu 20.04 / Kylin V10 SP1, glibc 2.31) ---'
        wails build -s -clean -skipbindings -platform linux/amd64 -o dirlocker -ldflags '-s -w'
        cp /workspace/build/bin/dirlocker /workspace/build-linux/dirlocker

        echo '--- Wails Windows 10/11 (x64, zero-CGO) ---'
        mkdir -p /workspace/build-win64
        wails build -s -skipbindings -platform windows/amd64 -o dirlocker.exe -ldflags '-s -w'
        cp /workspace/build/bin/dirlocker.exe /workspace/build-win64/dirlocker.exe

        echo '=== [3/4] Packaging Kylin V10 SP1 .deb & Manual Installer ==='
        KYLIN_DIST=/workspace/dist/kylin-ubuntu20.04
        rm -rf \"\${KYLIN_DIST}\"
        mkdir -p \"\${KYLIN_DIST}/installer\"
        bash /workspace/scripts/package_linux_deb.sh \
            /workspace/build-linux \
            \"\${KYLIN_DIST}/dirlocker_${APP_VERSION}_amd64.deb\"

        cp /workspace/build-linux/dirlockerd \
           /workspace/build-linux/dirlocker \
           /workspace/build-linux/libdirlocker-peony-plugin.so \
           /workspace/src/linux/packaging/dirlockerd.service \
           /workspace/src/linux/packaging/dirlocker.desktop \
           /workspace/src/linux/packaging/dirlocker-opener.desktop \
           /workspace/src/linux/packaging/dirlocker-autostart.desktop \
           /workspace/src/linux/packaging/dirlocker-mime.xml \
           /workspace/src/linux/packaging/install.sh \
           /workspace/src/linux/packaging/uninstall.sh \
           \"\${KYLIN_DIST}/installer/\"
        cp -r /workspace/src/linux/packaging/icons \"\${KYLIN_DIST}/installer/\"
        chmod 755 \"\${KYLIN_DIST}/installer/install.sh\" \"\${KYLIN_DIST}/installer/uninstall.sh\"

        echo '=== [4/4] Building Windows NSIS GUI Installer ==='
        WIN_DIST=/workspace/dist/windows
        rm -rf \"\${WIN_DIST}\"
        mkdir -p \"\${WIN_DIST}\"
        makensis -DAPP_VERSION=${APP_VERSION} \
            -DSRC_WIN_DIR=/workspace/src/windows \
            -DBIN_DIR=/workspace/build-win64 \
            -DOUT_FILE=\"\${WIN_DIST}/DirLocker_Setup_v${APP_VERSION}.exe\" \
            /workspace/src/windows/installer/dirlocker.nsi

        echo '=== Verify GLIBC ceiling & shared libs on Linux binaries ==='
        ldd /workspace/build-linux/dirlocker | head -n 20
        objdump -T /workspace/build-linux/dirlocker | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -n 5
        ls -lh \"\${KYLIN_DIST}/dirlocker_${APP_VERSION}_amd64.deb\" \"\${WIN_DIST}/DirLocker_Setup_v${APP_VERSION}.exe\"
    "
