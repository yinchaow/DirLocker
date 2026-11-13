#!/usr/bin/env bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "请使用 sudo 运行安装脚本: sudo $0"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 安装或更新前退出可能正在运行的旧版桌面程序，避免覆盖后旧版常驻内存
dirlocker --quit-silent 2>/dev/null || true
pkill -x dirlocker 2>/dev/null || true

install -Dm755 "${SCRIPT_DIR}/dirlockerd" /usr/sbin/dirlockerd
install -Dm755 "${SCRIPT_DIR}/dirlocker" /usr/bin/dirlocker
install -Dm644 "${SCRIPT_DIR}/libdirlocker-peony-plugin.so" /usr/lib/peony-qt-extensions/libdirlocker-peony-plugin.so

if [ -d /usr/lib/x86_64-linux-gnu/peony-qt-extensions ]; then
    ln -sf /usr/lib/peony-qt-extensions/libdirlocker-peony-plugin.so \
           /usr/lib/x86_64-linux-gnu/peony-qt-extensions/libdirlocker-peony-plugin.so
fi

install -Dm644 "${SCRIPT_DIR}/dirlockerd.service" /etc/systemd/system/dirlockerd.service
install -Dm644 "${SCRIPT_DIR}/dirlocker.desktop" /usr/share/applications/dirlocker.desktop
install -Dm644 "${SCRIPT_DIR}/dirlocker-opener.desktop" /usr/share/applications/dirlocker-opener.desktop
install -Dm644 "${SCRIPT_DIR}/dirlocker-autostart.desktop" /etc/xdg/autostart/dirlocker-autostart.desktop
install -Dm644 "${SCRIPT_DIR}/dirlocker-mime.xml" /usr/share/mime/packages/dirlocker.xml

if [ -d "${SCRIPT_DIR}/icons" ]; then
    for sz in 16x16 24x24 32x32 48x48 64x64 96x96 128x128 256x256; do
        if [ -f "${SCRIPT_DIR}/icons/apps/${sz}/dirlocker.png" ]; then
            install -Dm644 "${SCRIPT_DIR}/icons/apps/${sz}/dirlocker.png" \
                           "/usr/share/icons/hicolor/${sz}/apps/dirlocker.png"
        fi
        if [ -f "${SCRIPT_DIR}/icons/mimetypes/${sz}/dirlocker-folder.png" ]; then
            install -Dm644 "${SCRIPT_DIR}/icons/mimetypes/${sz}/dirlocker-folder.png" \
                           "/usr/share/icons/hicolor/${sz}/apps/dirlocker-folder.png"
            install -Dm644 "${SCRIPT_DIR}/icons/mimetypes/${sz}/dirlocker-folder.png" \
                           "/usr/share/icons/hicolor/${sz}/mimetypes/dirlocker-folder.png"
        fi
        if [ -f "${SCRIPT_DIR}/icons/mimetypes/${sz}/application-x-dirlocker-vault.png" ]; then
            install -Dm644 "${SCRIPT_DIR}/icons/mimetypes/${sz}/application-x-dirlocker-vault.png" \
                           "/usr/share/icons/hicolor/${sz}/mimetypes/application-x-dirlocker-vault.png"
        fi
    done
    if [ -f "${SCRIPT_DIR}/icons/apps/128x128/dirlocker.png" ]; then
        install -Dm644 "${SCRIPT_DIR}/icons/apps/128x128/dirlocker.png" /usr/share/pixmaps/dirlocker.png
    fi
    if [ -f "${SCRIPT_DIR}/icons/mimetypes/128x128/dirlocker-folder.png" ]; then
        install -Dm644 "${SCRIPT_DIR}/icons/mimetypes/128x128/dirlocker-folder.png" /usr/share/pixmaps/dirlocker-folder.png
    fi
fi

if [ -f "${SCRIPT_DIR}/RECOVERY.md" ]; then
    install -Dm644 "${SCRIPT_DIR}/RECOVERY.md" /usr/share/doc/dirlocker/RECOVERY.md
fi

if command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database /usr/share/mime >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

systemctl daemon-reload
systemctl enable --now dirlockerd.service

echo "目录密匣 安装完成！"
echo "1. 后台守护服务 dirlockerd 已启动并设为开机自启。"
echo "2. 如正在使用 Peony 文件管理器，可注销重登录或执行 'peony -q' 重启文管加载右键插件。"
echo "3. 可在应用菜单中打开「目录密匣」，或直接运行: dirlocker --show"

