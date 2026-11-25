//go:build windows

package backend

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"

	"dirlocker/internal/core"
	"golang.org/x/sys/windows/registry"
)

const (
	shcneAssocChanged = 0x08000000
	shcnfIDList       = 0x0000
	mbYesNo           = 0x00000004
	mbIconWarning     = 0x00000030
	mbDefButton2      = 0x00000100
	idYes             = 6
)

var (
	modUser32Reg    = syscall.NewLazyDLL("user32.dll")
	procMessageBoxW = modUser32Reg.NewProc("MessageBoxW")
)

func GetWindowsStatePath() string {
	local := os.Getenv("LOCALAPPDATA")
	if local == "" {
		home, _ := os.UserHomeDir()
		local = filepath.Join(home, "AppData", "Local")
	}
	return filepath.Join(local, "DirLocker", "state.json")
}

func getSelfExePath() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	return core.NormalizeWinPath(exe)
}

// resolveLockedFolderIconSpec returns the .dlock DefaultIcon registry value.
// Uses the side-by-side Fluent Design 2 folder_lock.ico installed alongside dirlocker.exe
// so Windows locked folders match Kylin's Fluent 2 dirlocker-folder.png icon identically.
func resolveLockedFolderIconSpec(exePath string) string {
	dir := filepath.Dir(exePath)
	ico := filepath.Join(dir, "folder_lock.ico")
	if fileExistsWin(ico) {
		return ico + ",0"
	}
	return exePath + ",0"
}

func canWriteHKLM() bool {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `Software\Classes`, registry.CREATE_SUB_KEY|registry.SET_VALUE)
	if err != nil {
		return false
	}
	_ = k.Close()
	return true
}

func setRegString(root registry.Key, subkey, name, val string) error {
	k, _, err := registry.CreateKey(root, subkey, registry.SET_VALUE)
	if err != nil {
		return err
	}
	defer k.Close()
	return k.SetStringValue(name, val)
}

func getRegString(root registry.Key, subkey, name string) (string, bool) {
	k, err := registry.OpenKey(root, subkey, registry.QUERY_VALUE)
	if err != nil {
		return "", false
	}
	defer k.Close()
	val, _, err := k.GetStringValue(name)
	if err != nil {
		return "", false
	}
	return val, true
}

func deleteRegTree(root registry.Key, subkey string) {
	k, err := registry.OpenKey(root, subkey, registry.ENUMERATE_SUB_KEYS|registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		return
	}
	subNames, _ := k.ReadSubKeyNames(-1)
	_ = k.Close()
	for _, sub := range subNames {
		deleteRegTree(root, subkey+`\`+sub)
	}
	_ = registry.DeleteKey(root, subkey)
}

func deleteRegValueSafe(root registry.Key, subkey, name string) {
	k, err := registry.OpenKey(root, subkey, registry.SET_VALUE)
	if err != nil {
		return
	}
	defer k.Close()
	_ = k.DeleteValue(name)
}

func writeDlockAssociation(hive registry.Key, exePath string) error {
	wantProgID := "DirLocker.LockedDir"
	wantIcon := resolveLockedFolderIconSpec(exePath)
	wantCmd := fmt.Sprintf(`"%s" --unlock-file "%%1"`, exePath)

	writes := [][3]string{
		{`Software\Classes\.dlock`, "", wantProgID},
		{`Software\Classes\.dlock\OpenWithProgids`, wantProgID, ""},
		{`Software\Classes\DirLocker.LockedDir`, "", "目录密匣加密目录"},
		{`Software\Classes\DirLocker.LockedDir`, "FriendlyTypeName", "目录密匣加密目录"},
		{`Software\Classes\DirLocker.LockedDir`, "NeverShowExt", ""},
		{`Software\Classes\DirLocker.LockedDir\DefaultIcon`, "", wantIcon},
		{`Software\Classes\DirLocker.LockedDir\shell`, "", "open"},
		{`Software\Classes\DirLocker.LockedDir\shell\open`, "", "使用 目录密匣 解锁打开"},
		{`Software\Classes\DirLocker.LockedDir\shell\open\command`, "", wantCmd},
	}
	for _, w := range writes {
		if err := setRegString(hive, w[0], w[1], w[2]); err != nil {
			return err
		}
	}
	procSHChangeNotify.Call(shcneAssocChanged, shcnfIDList|shcnfFlushNoWt, 0, 0)
	return nil
}

func WinEnsureDlockAssociation(exePath string) {
	if exePath == "" {
		return
	}
	wantProgID := "DirLocker.LockedDir"
	wantIcon := resolveLockedFolderIconSpec(exePath)
	wantCmd := fmt.Sprintf(`"%s" --unlock-file "%%1"`, exePath)

	curProgID, ok1 := getRegString(registry.CLASSES_ROOT, `.dlock`, "")
	_, ok2 := getRegString(registry.CLASSES_ROOT, `DirLocker.LockedDir`, "NeverShowExt")
	curIcon, ok3 := getRegString(registry.CLASSES_ROOT, `DirLocker.LockedDir\DefaultIcon`, "")
	curCmd, ok4 := getRegString(registry.CLASSES_ROOT, `DirLocker.LockedDir\shell\open\command`, "")
	if ok1 && ok2 && ok3 && ok4 &&
		curProgID == wantProgID &&
		strings.EqualFold(curIcon, wantIcon) &&
		strings.EqualFold(curCmd, wantCmd) {
		return
	}

	hive := registry.CURRENT_USER
	if canWriteHKLM() {
		hive = registry.LOCAL_MACHINE
		deleteRegTree(registry.CURRENT_USER, `Software\Classes\.dlock`)
		deleteRegTree(registry.CURRENT_USER, `Software\Classes\DirLocker.LockedDir`)
	}
	_ = writeDlockAssociation(hive, exePath)
}

func WinInstallIntegration(exePath string, withAutostart bool) error {
	if exePath == "" {
		return fmt.Errorf("程序路径为空")
	}
	machineScope := canWriteHKLM()
	hive := registry.CURRENT_USER
	if machineScope {
		hive = registry.LOCAL_MACHINE
		deleteRegTree(registry.CURRENT_USER, `Software\Classes\Directory\shell\DirLocker`)
		deleteRegTree(registry.CURRENT_USER, `Software\Classes\.dlock`)
		deleteRegTree(registry.CURRENT_USER, `Software\Classes\DirLocker.LockedDir`)
		deleteRegValueSafe(registry.CURRENT_USER, `Software\Microsoft\Windows\CurrentVersion\Run`, "DirLocker")
	}

	menuKey := `Software\Classes\Directory\shell\DirLocker`
	cmdKey := `Software\Classes\Directory\shell\DirLocker\command`
	if err := setRegString(hive, menuKey, "", "使用 目录密匣 锁定/解锁"); err != nil {
		return err
	}
	if err := setRegString(hive, menuKey, "Icon", fmt.Sprintf(`"%s",0`, exePath)); err != nil {
		return err
	}
	if err := setRegString(hive, cmdKey, "", fmt.Sprintf(`"%s" --toggle "%%1"`, exePath)); err != nil {
		return err
	}
	if err := writeDlockAssociation(hive, exePath); err != nil {
		return err
	}

	runKey := `Software\Microsoft\Windows\CurrentVersion\Run`
	if withAutostart {
		_ = setRegString(hive, runKey, "DirLocker", fmt.Sprintf(`"%s" --tray`, exePath))
	} else {
		deleteRegValueSafe(hive, runKey, "DirLocker")
	}
	return nil
}

func WinUninstallIntegration() {
	deleteRegTree(registry.CURRENT_USER, `Software\Classes\Directory\shell\DirLocker`)
	deleteRegTree(registry.CURRENT_USER, `Software\Classes\.dlock`)
	deleteRegTree(registry.CURRENT_USER, `Software\Classes\DirLocker.LockedDir`)
	deleteRegValueSafe(registry.CURRENT_USER, `Software\Microsoft\Windows\CurrentVersion\Run`, "DirLocker")
	if canWriteHKLM() {
		deleteRegTree(registry.LOCAL_MACHINE, `Software\Classes\Directory\shell\DirLocker`)
		deleteRegTree(registry.LOCAL_MACHINE, `Software\Classes\.dlock`)
		deleteRegTree(registry.LOCAL_MACHINE, `Software\Classes\DirLocker.LockedDir`)
		deleteRegValueSafe(registry.LOCAL_MACHINE, `Software\Microsoft\Windows\CurrentVersion\Run`, "DirLocker")
	}
	procSHChangeNotify.Call(shcneAssocChanged, shcnfIDList|shcnfFlushNoWt, 0, 0)
}

func winShowMessageBox(title, body string, flags uint32) int {
	wt, _ := syscall.UTF16PtrFromString(title)
	wb, _ := syscall.UTF16PtrFromString(body)
	r, _, _ := procMessageBoxW.Call(0, uintptr(unsafe.Pointer(wb)), uintptr(unsafe.Pointer(wt)), uintptr(flags))
	return int(r)
}

func winConfirmUninstallIfLocked(statePath string) bool {
	store := core.NewStateStore(statePath)
	_ = store.Load()
	lockedCount := 0
	for _, e := range store.State.Entries {
		if e.Locked {
			lockedCount++
		}
	}
	if lockedCount == 0 {
		return true
	}
	msg := fmt.Sprintf(
		"检测到当前仍有 %d 个目录处于锁定状态。\n\n"+
			"卸载目录密匣不会删除这些目录的数据（数据仍安全保存在磁盘保护区中），但卸载后您将无法通过双击解锁，建议先打开 目录密匣 全部解锁后再卸载。\n\n"+
			"确定仍要继续卸载吗？",
		lockedCount,
	)
	return winShowMessageBox("卸载确认 — 目录密匣", msg, mbYesNo|mbIconWarning|mbDefButton2) == idYes
}
