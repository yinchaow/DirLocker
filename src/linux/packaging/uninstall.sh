#!/usr/bin/env bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "请使用 sudo 运行卸载脚本: sudo $0"
    exit 1
fi

if ls /var/lib/dirlocker/uid_*.json >/dev/null 2>&1; then
    if grep -q '"locked":[[:space:]]*true' /var/lib/dirlocker/uid_*.json 2>/dev/null; then
        echo "警告：检测到仍有处于锁定状态的目录！"
        echo "建议先打开 DirLocker 解锁所有目录再卸载。如需继续卸载，文件不会丢失，可按 RECOVERY.md 手动恢复权限。"
        read -r -p "确定要继续卸载吗？[y/N] " ans
        case "${ans}" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "已取消卸载。"; exit 0 ;;
        esac
    fi
fi

pkill -f '/usr/bin/dirlocker' 2>/dev/null || true
systemctl disable --now dirlockerd.service 2>/dev/null || true

rm -f /etc/systemd/system/dirlockerd.service
rm -f /usr/sbin/dirlockerd
rm -f /usr/bin/dirlocker
rm -f /usr/lib/peony-qt-extensions/libdirlocker-peony-plugin.so
rm -f /usr/lib/x86_64-linux-gnu/peony-qt-extensions/libdirlocker-peony-plugin.so
rm -f /usr/share/applications/dirlocker.desktop
rm -f /usr/share/applications/dirlocker-opener.desktop
rm -f /etc/xdg/autostart/dirlocker-autostart.desktop
rm -f /usr/share/mime/packages/dirlocker.xml
rm -f /usr/share/pixmaps/dirlocker.png /usr/share/pixmaps/dirlocker-folder.png
rm -f /usr/share/icons/hicolor/*/apps/dirlocker.png \
      /usr/share/icons/hicolor/*/apps/dirlocker-folder.png \
      /usr/share/icons/hicolor/*/mimetypes/dirlocker-folder.png \
      /usr/share/icons/hicolor/*/mimetypes/application-x-dirlocker-vault.png

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
echo "DirLocker 程序已卸载（/var/lib/dirlocker 状态配置保留，如需清理可手动删除）。"

