#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:?Usage: $0 <linux_build_dir> <output_deb_path>}"
OUT_DEB="${2:?Usage: $0 <linux_build_dir> <output_deb_path>}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PKG_ROOT="$(mktemp -d)"
chmod 0755 "${PKG_ROOT}"
trap 'rm -rf "${PKG_ROOT}"' EXIT

mkdir -p "${PKG_ROOT}/DEBIAN" \
         "${PKG_ROOT}/usr/sbin" \
         "${PKG_ROOT}/usr/bin" \
         "${PKG_ROOT}/usr/lib/peony-qt-extensions" \
         "${PKG_ROOT}/etc/systemd/system" \
         "${PKG_ROOT}/usr/share/applications" \
         "${PKG_ROOT}/usr/share/mime/packages" \
         "${PKG_ROOT}/usr/share/pixmaps" \
         "${PKG_ROOT}/etc/xdg/autostart"

install -m 755 "${BUILD_DIR}/dirlockerd" "${PKG_ROOT}/usr/sbin/dirlockerd"
install -m 755 "${BUILD_DIR}/dirlocker" "${PKG_ROOT}/usr/bin/dirlocker"
if [ -f "${BUILD_DIR}/libdirlocker-peony-plugin.so" ]; then
    install -m 644 "${BUILD_DIR}/libdirlocker-peony-plugin.so" \
                   "${PKG_ROOT}/usr/lib/peony-qt-extensions/libdirlocker-peony-plugin.so"
fi

install -m 644 "${ROOT_DIR}/src/linux/packaging/dirlockerd.service" \
               "${PKG_ROOT}/etc/systemd/system/dirlockerd.service"
install -m 644 "${ROOT_DIR}/src/linux/packaging/dirlocker.desktop" \
               "${PKG_ROOT}/usr/share/applications/dirlocker.desktop"
install -m 644 "${ROOT_DIR}/src/linux/packaging/dirlocker-opener.desktop" \
               "${PKG_ROOT}/usr/share/applications/dirlocker-opener.desktop"
install -m 644 "${ROOT_DIR}/src/linux/packaging/dirlocker-autostart.desktop" \
               "${PKG_ROOT}/etc/xdg/autostart/dirlocker-autostart.desktop"
install -m 644 "${ROOT_DIR}/src/linux/packaging/dirlocker-mime.xml" \
               "${PKG_ROOT}/usr/share/mime/packages/dirlocker.xml"

ICON_SRC="${ROOT_DIR}/src/linux/packaging/icons"
for sz in 16x16 24x24 32x32 48x48 64x64 96x96 128x128 256x256; do
    install -Dm644 "${ICON_SRC}/apps/${sz}/dirlocker.png" \
                   "${PKG_ROOT}/usr/share/icons/hicolor/${sz}/apps/dirlocker.png"
    install -Dm644 "${ICON_SRC}/mimetypes/${sz}/dirlocker-folder.png" \
                   "${PKG_ROOT}/usr/share/icons/hicolor/${sz}/apps/dirlocker-folder.png"
    install -Dm644 "${ICON_SRC}/mimetypes/${sz}/dirlocker-folder.png" \
                   "${PKG_ROOT}/usr/share/icons/hicolor/${sz}/mimetypes/dirlocker-folder.png"
    install -Dm644 "${ICON_SRC}/mimetypes/${sz}/application-x-dirlocker-vault.png" \
                   "${PKG_ROOT}/usr/share/icons/hicolor/${sz}/mimetypes/application-x-dirlocker-vault.png"
done
install -Dm644 "${ICON_SRC}/apps/128x128/dirlocker.png" \
               "${PKG_ROOT}/usr/share/pixmaps/dirlocker.png"
install -Dm644 "${ICON_SRC}/mimetypes/128x128/dirlocker-folder.png" \
               "${PKG_ROOT}/usr/share/pixmaps/dirlocker-folder.png"

cat > "${PKG_ROOT}/DEBIAN/control" <<'EOF'
Package: dirlocker
Version: 2.2.1
Section: utils
Priority: optional
Architecture: amd64
Maintainer: DirLocker <dirlocker@local>
Depends: libc6 (>= 2.31), libstdc++6 (>= 9), libqt5core5a (>= 5.12), libqt5gui5 (>= 5.12), libqt5widgets5 (>= 5.12), libqt5network5 (>= 5.12), libqt5x11extras5 (>= 5.12), libx11-6, libxss1
Description: DirLocker lightweight password lock for directories
 Protects directories on Kylin V10 SP1 / Ubuntu 20.04 with global master
 password or per-directory custom password, Peony context menu, custom hotkey
 lock, idle auto-relock, occupancy protection, and vault health repair.
EOF

cat > "${PKG_ROOT}/DEBIAN/preinst" <<'EOF'
#!/bin/sh
set -e
# 安装或升级前，退出可能在用户桌面运行的旧版 GUI，避免覆盖后旧版常驻内存
dirlocker --quit-silent 2>/dev/null || true
pkill -x dirlocker 2>/dev/null || true
exit 0
EOF

cat > "${PKG_ROOT}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ -d /usr/lib/x86_64-linux-gnu/peony-qt-extensions ] && [ -f /usr/lib/peony-qt-extensions/libdirlocker-peony-plugin.so ]; then
    ln -sf /usr/lib/peony-qt-extensions/libdirlocker-peony-plugin.so \
           /usr/lib/x86_64-linux-gnu/peony-qt-extensions/libdirlocker-peony-plugin.so
fi
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
    systemctl enable dirlockerd.service || true
    systemctl restart dirlockerd.service || true
fi
if command -v update-mime-database >/dev/null 2>&1 && [ -d /usr/share/mime ]; then
    update-mime-database /usr/share/mime >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1 && [ -d /usr/share/applications ]; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d /usr/share/icons/hicolor ]; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
exit 0
EOF

cat > "${PKG_ROOT}/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if ls /var/lib/dirlocker/uid_*.json >/dev/null 2>&1; then
        if grep -q '"locked":[[:space:]]*true' /var/lib/dirlocker/uid_*.json 2>/dev/null; then
            echo "WARNING: DirLocker still has locked directories. Data is intact; unlock them via DirLocker or admin permissions." >&2
        fi
    fi
    pkill -f '/usr/bin/dirlocker' 2>/dev/null || true
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        systemctl stop dirlockerd.service || true
        systemctl disable dirlockerd.service || true
    fi
fi
exit 0
EOF

cat > "${PKG_ROOT}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
rm -f /usr/lib/x86_64-linux-gnu/peony-qt-extensions/libdirlocker-peony-plugin.so
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
fi
if command -v update-mime-database >/dev/null 2>&1 && [ -d /usr/share/mime ]; then
    update-mime-database /usr/share/mime >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1 && [ -d /usr/share/applications ]; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d /usr/share/icons/hicolor ]; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
exit 0
EOF

chmod 755 "${PKG_ROOT}/DEBIAN/preinst" "${PKG_ROOT}/DEBIAN/postinst" "${PKG_ROOT}/DEBIAN/prerm" "${PKG_ROOT}/DEBIAN/postrm"
mkdir -p "$(dirname "${OUT_DEB}")"
dpkg-deb --build --root-owner-group "${PKG_ROOT}" "${OUT_DEB}"
echo "Built DEB package: ${OUT_DEB}"

