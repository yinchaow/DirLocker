//go:build windows

package backend

import (
	"encoding/json"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"dirlocker/internal/core"
	"github.com/go-ole/go-ole"
	"github.com/go-ole/go-ole/oleutil"
	"golang.org/x/sys/windows"
)

const (
	fileListDirectory   uint32 = 0x0001
	fileAddFile         uint32 = 0x0002
	fileAddSubdirectory uint32 = 0x0004
	fileReadEA          uint32 = 0x0008
	fileWriteEA         uint32 = 0x0010
	fileTraverse        uint32 = 0x0020
	fileDeleteChild     uint32 = 0x0040
	fileWriteAttributes uint32 = 0x0100
	fileWriteData       uint32 = 0x0002
	fileAppendData      uint32 = 0x0004
	standardDelete      uint32 = 0x00010000

	kLockDenyMask = fileListDirectory |
		fileAddFile |
		fileAddSubdirectory |
		fileReadEA |
		fileWriteEA |
		fileTraverse |
		fileDeleteChild |
		standardDelete |
		fileWriteAttributes

	kStubDenyMask = standardDelete |
		fileWriteData |
		fileAppendData |
		fileWriteAttributes |
		fileWriteEA

	accessDeniedAceType = 1
	inheritedAceFlag    = 0x10
	aclRevision         = 2
	aclRevisionInfo     = 1
	aclSizeInfo         = 2

	shcneUpdateDir  = 0x00001000
	shcnfPathW      = 0x0005
	shcnfFlushNoWt  = 0x2000
	driveFixed      = 3
	driveRemovable  = 2
	rmMaxAppName    = 255
	rmMaxSvcName    = 63
	cchRmSessionKey = 32
)

var (
	modAdvapi32           = syscall.NewLazyDLL("advapi32.dll")
	modKernel32           = syscall.NewLazyDLL("kernel32.dll")
	modShell32            = syscall.NewLazyDLL("shell32.dll")
	modRstrtmgr           = syscall.NewLazyDLL("rstrtmgr.dll")
	procGetAclInformation = modAdvapi32.NewProc("GetAclInformation")
	procGetAce            = modAdvapi32.NewProc("GetAce")
	procInitializeAcl     = modAdvapi32.NewProc("InitializeAcl")
	procAddAce            = modAdvapi32.NewProc("AddAce")
	procEqualSid          = modAdvapi32.NewProc("EqualSid")
	procSHChangeNotify    = modShell32.NewProc("SHChangeNotify")
	procGetLogicalDrives  = modKernel32.NewProc("GetLogicalDrives")
	procGetDriveTypeW     = modKernel32.NewProc("GetDriveTypeW")
	procRmStartSession    = modRstrtmgr.NewProc("RmStartSession")
	procRmRegisterRes     = modRstrtmgr.NewProc("RmRegisterResources")
	procRmGetList         = modRstrtmgr.NewProc("RmGetList")
	procRmEndSession      = modRstrtmgr.NewProc("RmEndSession")
)

type aclSizeInformation struct {
	AceCount      uint32
	AclBytesInUse uint32
	AclBytesFree  uint32
}

type aclRevisionInformation struct {
	AclRevision uint32
}

type aceHeader struct {
	AceType  byte
	AceFlags byte
	AceSize  uint16
}

type accessDeniedAce struct {
	Header   aceHeader
	Mask     uint32
	SidStart uint32
}

type rmUniqueProcess struct {
	ProcessId        uint32
	ProcessStartTime windows.Filetime
}

type rmProcessInfo struct {
	Process          rmUniqueProcess
	AppName          [rmMaxAppName + 1]uint16
	ServiceShortName [rmMaxSvcName + 1]uint16
	ApplicationType  uint32
	AppStatus        uint32
	TSSessionId      uint32
	Restartable      int32
}

func formatWinErr(prefix string, err error) string {
	if err == windows.ERROR_ACCESS_DENIED {
		return prefix + ": 权限不足（请尝试以管理员身份运行 目录密匣）"
	}
	if errno, ok := err.(syscall.Errno); ok {
		return fmt.Sprintf("%s (Win32 错误码: %d)", prefix, uint32(errno))
	}
	return fmt.Sprintf("%s: %v", prefix, err)
}

func resolveTargetSID(sidStr string) (*windows.SID, error) {
	if sidStr != "" {
		if sid, err := windows.StringToSid(sidStr); err == nil {
			return sid, nil
		}
	}
	return windows.CreateWellKnownSid(windows.WinWorldSid)
}

func hasDenyAceMask(dacl *windows.ACL, targetSID *windows.SID, denyMask uint32) bool {
	if dacl == nil || targetSID == nil {
		return false
	}
	var info aclSizeInformation
	r, _, _ := procGetAclInformation.Call(
		uintptr(unsafe.Pointer(dacl)),
		uintptr(unsafe.Pointer(&info)),
		unsafe.Sizeof(info),
		aclSizeInfo,
	)
	if r == 0 {
		return false
	}
	for i := uint32(0); i < info.AceCount; i++ {
		var acePtr uintptr
		ok, _, _ := procGetAce.Call(uintptr(unsafe.Pointer(dacl)), uintptr(i), uintptr(unsafe.Pointer(&acePtr)))
		if ok == 0 || acePtr == 0 {
			continue
		}
		hdr := (*aceHeader)(unsafe.Pointer(acePtr))
		if hdr.AceType != accessDeniedAceType || (hdr.AceFlags&inheritedAceFlag) != 0 {
			continue
		}
		denyAce := (*accessDeniedAce)(unsafe.Pointer(acePtr))
		sidPtr := uintptr(unsafe.Pointer(&denyAce.SidStart))
		eq, _, _ := procEqualSid.Call(sidPtr, uintptr(unsafe.Pointer(targetSID)))
		if eq != 0 && (denyAce.Mask&denyMask) == denyMask {
			return true
		}
	}
	return false
}

func applyDenyAce(path string, denyMask uint32, usedSID *string) error {
	if *usedSID == "" {
		*usedSID = "S-1-1-0"
	}
	targetSID, err := resolveTargetSID(*usedSID)
	if err != nil {
		return fmt.Errorf("无法构造安全标识符 (SID): %w", err)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("%s", formatWinErr("读取 ACL 失败", err))
	}
	oldDACL, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("%s", formatWinErr("解析 ACL 失败", err))
	}
	if hasDenyAceMask(oldDACL, targetSID, denyMask) {
		return nil
	}

	entries := []windows.EXPLICIT_ACCESS{
		{
			AccessPermissions: windows.ACCESS_MASK(denyMask),
			AccessMode:        windows.DENY_ACCESS,
			Inheritance:       windows.NO_INHERITANCE,
			Trustee: windows.TRUSTEE{
				TrusteeForm:  windows.TRUSTEE_IS_SID,
				TrusteeType:  windows.TRUSTEE_IS_WELL_KNOWN_GROUP,
				TrusteeValue: windows.TrusteeValueFromSID(targetSID),
			},
		},
	}
	newDACL, err := windows.ACLFromEntries(entries, oldDACL)
	if err != nil {
		return fmt.Errorf("%s", formatWinErr("构建保护 ACL 失败", err))
	}
	err = windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION, nil, nil, newDACL, nil)
	if err != nil {
		return fmt.Errorf("%s", formatWinErr("写入保护权限失败", err))
	}
	return nil
}

func removeDenyAce(path string, denyMask uint32, usedSID string) error {
	if usedSID == "" {
		usedSID = "S-1-1-0"
	}
	targetSID, err := resolveTargetSID(usedSID)
	if err != nil {
		return fmt.Errorf("无法解析安全标识符 (SID): %w", err)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("%s", formatWinErr("读取 ACL 失败", err))
	}
	oldDACL, _, err := sd.DACL()
	if err != nil || oldDACL == nil {
		return nil
	}

	var sizeInfo aclSizeInformation
	ok, _, _ := procGetAclInformation.Call(
		uintptr(unsafe.Pointer(oldDACL)),
		uintptr(unsafe.Pointer(&sizeInfo)),
		unsafe.Sizeof(sizeInfo),
		aclSizeInfo,
	)
	if ok == 0 {
		return fmt.Errorf("获取 ACL 大小信息失败")
	}
	revInfo := aclRevisionInformation{AclRevision: aclRevision}
	procGetAclInformation.Call(
		uintptr(unsafe.Pointer(oldDACL)),
		uintptr(unsafe.Pointer(&revInfo)),
		unsafe.Sizeof(revInfo),
		aclRevisionInfo,
	)

	allocSize := sizeInfo.AclBytesInUse
	if allocSize < 64 {
		allocSize = 64
	}
	buf := make([]byte, allocSize)
	newDACLPtr := uintptr(unsafe.Pointer(&buf[0]))
	ok, _, _ = procInitializeAcl.Call(newDACLPtr, uintptr(allocSize), uintptr(revInfo.AclRevision))
	if ok == 0 {
		return fmt.Errorf("初始化新 ACL 失败")
	}

	removedAny := false
	for i := uint32(0); i < sizeInfo.AceCount; i++ {
		var acePtr uintptr
		r, _, _ := procGetAce.Call(uintptr(unsafe.Pointer(oldDACL)), uintptr(i), uintptr(unsafe.Pointer(&acePtr)))
		if r == 0 || acePtr == 0 {
			continue
		}
		hdr := (*aceHeader)(unsafe.Pointer(acePtr))
		shouldRemove := false
		if hdr.AceType == accessDeniedAceType && (hdr.AceFlags&inheritedAceFlag) == 0 {
			denyAce := (*accessDeniedAce)(unsafe.Pointer(acePtr))
			eq, _, _ := procEqualSid.Call(uintptr(unsafe.Pointer(&denyAce.SidStart)), uintptr(unsafe.Pointer(targetSID)))
			if eq != 0 && denyAce.Mask == denyMask {
				shouldRemove = true
			}
		}
		if shouldRemove {
			removedAny = true
			continue
		}
		r, _, _ = procAddAce.Call(newDACLPtr, uintptr(revInfo.AclRevision), 0xFFFFFFFF, acePtr, uintptr(hdr.AceSize))
		if r == 0 {
			return fmt.Errorf("复制原有权限条目失败")
		}
	}

	if removedAny {
		newDACL := (*windows.ACL)(unsafe.Pointer(&buf[0]))
		err = windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION, nil, nil, newDACL, nil)
		if err != nil {
			return fmt.Errorf("%s", formatWinErr("恢复目录原始权限失败", err))
		}
	}
	runtime.KeepAlive(buf)
	return nil
}

func winIsDirectoryLocked(path, usedSID string) bool {
	if usedSID == "" {
		usedSID = "S-1-1-0"
	}
	targetSID, err := resolveTargetSID(usedSID)
	if err != nil {
		return false
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return false
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return false
	}
	return hasDenyAceMask(dacl, targetSID, kLockDenyMask)
}

func parentOfWinPath(p string) string {
	p = core.NormalizeWinPath(p)
	idx := strings.LastIndexAny(p, `\/`)
	if idx < 0 {
		return ""
	}
	if idx == 2 && len(p) >= 3 && p[1] == ':' {
		return p[:3]
	}
	return p[:idx]
}

func notifyShellParentUpdated(p string) {
	parent := parentOfWinPath(p)
	if parent == "" {
		return
	}
	wp, err := syscall.UTF16PtrFromString(parent)
	if err != nil {
		return
	}
	procSHChangeNotify.Call(shcneUpdateDir, shcnfPathW|shcnfFlushNoWt, uintptr(unsafe.Pointer(wp)), 0)
}

func getFileAttrsWin(p string) (uint32, bool) {
	wp, err := syscall.UTF16PtrFromString(p)
	if err != nil {
		return 0, false
	}
	attrs, err := windows.GetFileAttributes(wp)
	if err != nil || attrs == windows.INVALID_FILE_ATTRIBUTES {
		return 0, false
	}
	return attrs, true
}

func setFileAttrsWin(p string, attrs uint32) error {
	wp, err := syscall.UTF16PtrFromString(p)
	if err != nil {
		return err
	}
	return windows.SetFileAttributes(wp, attrs)
}

func dirExistsWin(p string) bool {
	if p == "" {
		return false
	}
	attrs, ok := getFileAttrsWin(p)
	return ok && (attrs&windows.FILE_ATTRIBUTE_DIRECTORY) != 0
}

func fileExistsWin(p string) bool {
	if p == "" {
		return false
	}
	attrs, ok := getFileAttrsWin(p)
	return ok && (attrs&windows.FILE_ATTRIBUTE_DIRECTORY) == 0
}

func isWinDriveReady(p string) bool {
	if len(p) < 2 || p[1] != ':' {
		return true
	}
	drv := strings.ToUpper(p[:1])
	if drv[0] < 'A' || drv[0] > 'Z' {
		return true
	}
	_, ok := getFileAttrsWin(drv + `:\`)
	return ok
}

func isUNCOrNonDriveWin(p string) bool {
	return len(p) < 3 || p[1] != ':'
}

func ensureHiddenDirUnlocked(dirPath string) bool {
	if err := os.MkdirAll(dirPath, 0700); err != nil {
		if !dirExistsWin(dirPath) {
			return false
		}
	}
	_ = removeDenyAce(dirPath, kLockDenyMask, "S-1-1-0")
	_ = setFileAttrsWin(dirPath, windows.FILE_ATTRIBUTE_HIDDEN|windows.FILE_ATTRIBUTE_SYSTEM)
	return true
}

func isDirEmptyWin(dirPath string) bool {
	entries, err := os.ReadDir(dirPath)
	if err != nil {
		return false
	}
	return len(entries) == 0
}

func sealVaultRootIfNonEmpty(vaultRoot string, cleanupIfEmpty bool) {
	if vaultRoot == "" {
		return
	}
	_ = removeDenyAce(vaultRoot, kLockDenyMask, "S-1-1-0")
	if cleanupIfEmpty && isDirEmptyWin(vaultRoot) {
		_ = setFileAttrsWin(vaultRoot, windows.FILE_ATTRIBUTE_DIRECTORY)
		if os.Remove(vaultRoot) == nil {
			return
		}
	}
	if !isDirEmptyWin(vaultRoot) {
		_ = setFileAttrsWin(vaultRoot, windows.FILE_ATTRIBUTE_HIDDEN|windows.FILE_ATTRIBUTE_SYSTEM)
		sid := "S-1-1-0"
		_ = applyDenyAce(vaultRoot, kLockDenyMask, &sid)
	}
}

func deleteProtectedFile(filePath string) bool {
	if filePath == "" {
		return true
	}
	if _, ok := getFileAttrsWin(filePath); !ok {
		return true
	}
	_ = removeDenyAce(filePath, kStubDenyMask, "S-1-1-0")
	_ = setFileAttrsWin(filePath, windows.FILE_ATTRIBUTE_NORMAL)
	return os.Remove(filePath) == nil
}

func winMakeCompanionDlockPath(dirPath string) string {
	return core.NormalizeWinPath(dirPath) + "（已锁定，点此解锁）.dlock"
}

type dlockStubDoc struct {
	App     string `json:"app"`
	Version int    `json:"version"`
	ID      string `json:"id"`
	Path    string `json:"path"`
	Tip     string `json:"tip"`
}

func createLockedDlockStub(dlockPath string, entry core.DirEntry) bool {
	deleteProtectedFile(dlockPath)
	doc := dlockStubDoc{
		App:     "目录密匣",
		Version: 1,
		ID:      entry.ID,
		Path:    entry.Path,
		Tip:     "此文件为“目录密匣”加密目录入口，双击输入密码即可解锁恢复，请勿手工删除。",
	}
	raw, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return false
	}
	raw = append(raw, '\n')
	if err := os.WriteFile(dlockPath, raw, 0644); err != nil {
		return false
	}
	_ = setFileAttrsWin(dlockPath, windows.FILE_ATTRIBUTE_READONLY)
	sid := "S-1-1-0"
	_ = applyDenyAce(dlockPath, kStubDenyMask, &sid)
	return true
}

func WinReadDlockFile(dlockPath string) (string, string, bool) {
	raw, err := os.ReadFile(dlockPath)
	if err != nil || len(raw) == 0 || len(raw) > 65536 {
		return "", "", false
	}
	var doc dlockStubDoc
	if err := json.Unmarshal(raw, &doc); err != nil {
		return "", "", false
	}
	if doc.ID == "" && doc.Path == "" {
		return "", "", false
	}
	return doc.ID, doc.Path, true
}

func chooseSameVolumeVault(origPath, stateDir, entryID string) (string, bool) {
	if isUNCOrNonDriveWin(origPath) {
		return "", false
	}
	origDrv := strings.ToUpper(origPath[:1])
	stateDrv := ""
	if len(stateDir) >= 2 && stateDir[1] == ':' {
		stateDrv = strings.ToUpper(stateDir[:1])
	}
	if origDrv == stateDrv && stateDir != "" {
		vroot := filepath.Join(stateDir, "Vault")
		if ensureHiddenDirUnlocked(vroot) {
			return filepath.Join(vroot, entryID), true
		}
	}
	drvVroot := origDrv + `:\$目录密匣_重要数据保护区_切勿删除`
	if ensureHiddenDirUnlocked(drvVroot) {
		return filepath.Join(drvVroot, entryID), true
	}
	parent := parentOfWinPath(origPath)
	if parent != "" {
		return filepath.Join(parent, ".dirvault_"+entryID), true
	}
	return "", false
}

func moveDirWin(src, dst string) error {
	wsrc, err := syscall.UTF16PtrFromString(src)
	if err != nil {
		return err
	}
	wdst, err := syscall.UTF16PtrFromString(dst)
	if err != nil {
		return err
	}
	return windows.MoveFile(wsrc, wdst)
}

func pushUniqueProc(out *[]string, seen map[string]struct{}, label string) {
	if label == "" || len(*out) >= 6 {
		return
	}
	key := strings.ToLower(label)
	if _, exists := seen[key]; !exists {
		seen[key] = struct{}{}
		*out = append(*out, label)
	}
}

func decodeFileURLToWinPath(rawURL string) string {
	if !strings.HasPrefix(strings.ToLower(rawURL), "file://") {
		return ""
	}
	rest := rawURL[7:]
	decoded, err := url.PathUnescape(rest)
	if err != nil {
		decoded = rest
	}
	if strings.HasPrefix(decoded, "/") {
		return core.NormalizeWinPath(decoded[1:])
	}
	return core.NormalizeWinPath(`\\` + decoded)
}

func collectExplorerBlockingWindows(targetPath string, out *[]string, seen map[string]struct{}) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	if err := ole.CoInitializeEx(0, ole.COINIT_APARTMENTTHREADED); err != nil {
		// S_FALSE or already initialized in another mode is fine to proceed/ignore
		_ = err
	} else {
		defer ole.CoUninitialize()
	}

	clsidShellWindows := ole.NewGUID("{9BA05972-F6A8-11CF-A442-00A0C90A8F39}")
	unknown, err := ole.CreateInstance(clsidShellWindows, ole.IID_IDispatch)
	if err != nil || unknown == nil {
		return
	}
	disp := unknown.MustQueryInterface(ole.IID_IDispatch)
	defer disp.Release()

	countVar, err := oleutil.GetProperty(disp, "Count")
	if err != nil {
		return
	}
	count := int(countVar.Val)
	countVar.Clear()

	for i := 0; i < count; i++ {
		itemVar, err := oleutil.CallMethod(disp, "Item", i)
		if err != nil || itemVar == nil {
			continue
		}
		itemDisp := itemVar.ToIDispatch()
		if itemDisp != nil {
			locVar, err := oleutil.GetProperty(itemDisp, "LocationURL")
			if err == nil && locVar != nil {
				loc := locVar.ToString()
				locVar.Clear()
				winLoc := decodeFileURLToWinPath(loc)
				if winLoc != "" && core.IsSameOrSubpath(winLoc, targetPath) {
					pushUniqueProc(out, seen, "explorer.exe (文件资源管理器窗口)")
				}
			}
		}
		itemVar.Clear()
	}
}

func collectRMCandidateFiles(targetPath string) (busy []string, sample []string) {
	type node struct {
		dir   string
		depth int
	}
	stack := []node{{dir: core.NormalizeWinPath(targetPath), depth: 0}}
	const (
		maxDepth   = 8
		maxScanned = 512
		maxBusy    = 64
		maxSamples = 24
	)
	scanned := 0
	for len(stack) > 0 && scanned < maxScanned && len(busy) < maxBusy {
		cur := stack[len(stack)-1]
		stack = stack[:len(stack)-1]

		entries, err := os.ReadDir(cur.dir)
		if err != nil {
			continue
		}
		for _, ent := range entries {
			full := filepath.Join(cur.dir, ent.Name())
			attrs, ok := getFileAttrsWin(full)
			if !ok || (attrs&windows.FILE_ATTRIBUTE_REPARSE_POINT) != 0 {
				continue
			}
			if (attrs & windows.FILE_ATTRIBUTE_DIRECTORY) != 0 {
				if cur.depth < maxDepth {
					stack = append(stack, node{dir: full, depth: cur.depth + 1})
				}
				continue
			}
			scanned++
			wfull, err := syscall.UTF16PtrFromString(full)
			if err == nil {
				h, cerr := windows.CreateFile(wfull, windows.GENERIC_READ, 0, nil, windows.OPEN_EXISTING, windows.FILE_ATTRIBUTE_NORMAL, 0)
				if cerr == nil {
					_ = windows.CloseHandle(h)
					if len(sample) < maxSamples {
						sample = append(sample, full)
					}
				} else if cerr == windows.ERROR_SHARING_VIOLATION {
					busy = append(busy, full)
				}
			}
			if scanned >= maxScanned || len(busy) >= maxBusy {
				break
			}
		}
	}
	return busy, sample
}

func queryProcExeBasename(pid uint32) string {
	h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
	if err != nil {
		return ""
	}
	defer windows.CloseHandle(h)
	var buf [520]uint16
	size := uint32(len(buf))
	if err := windows.QueryFullProcessImageName(h, 0, &buf[0], &size); err != nil || size == 0 {
		return ""
	}
	full := syscall.UTF16ToString(buf[:size])
	return filepath.Base(full)
}

func collectRMBlockingProcesses(targetPath string, out *[]string, seen map[string]struct{}) {
	if procRmStartSession.Find() != nil {
		return
	}
	busy, sample := collectRMCandidateFiles(targetPath)
	candidates := busy
	if len(candidates) == 0 {
		candidates = sample
	}
	if len(candidates) == 0 {
		return
	}

	var sess uint32
	var sessKey [cchRmSessionKey + 1]uint16
	r, _, _ := procRmStartSession.Call(uintptr(unsafe.Pointer(&sess)), 0, uintptr(unsafe.Pointer(&sessKey[0])))
	if r != 0 {
		return
	}
	defer procRmEndSession.Call(uintptr(sess))

	ptrs := make([]*uint16, 0, len(candidates))
	for _, c := range candidates {
		if wp, err := syscall.UTF16PtrFromString(c); err == nil {
			ptrs = append(ptrs, wp)
		}
	}
	if len(ptrs) == 0 {
		return
	}

	r, _, _ = procRmRegisterRes.Call(
		uintptr(sess),
		uintptr(len(ptrs)),
		uintptr(unsafe.Pointer(&ptrs[0])),
		0, 0, 0, 0,
	)
	if r != 0 {
		return
	}

	var needed uint32
	var reboot uint32
	infos := make([]rmProcessInfo, 16)
	count := uint32(len(infos))
	r, _, _ = procRmGetList.Call(
		uintptr(sess),
		uintptr(unsafe.Pointer(&needed)),
		uintptr(unsafe.Pointer(&count)),
		uintptr(unsafe.Pointer(&infos[0])),
		uintptr(unsafe.Pointer(&reboot)),
	)
	for attempt := 0; r == uintptr(windows.ERROR_MORE_DATA) && attempt < 3; attempt++ {
		if needed == 0 {
			needed = 32
		}
		infos = make([]rmProcessInfo, needed)
		count = uint32(len(infos))
		r, _, _ = procRmGetList.Call(
			uintptr(sess),
			uintptr(unsafe.Pointer(&needed)),
			uintptr(unsafe.Pointer(&count)),
			uintptr(unsafe.Pointer(&infos[0])),
			uintptr(unsafe.Pointer(&reboot)),
		)
	}
	if r != 0 {
		return
	}

	selfPID := uint32(os.Getpid())
	for i := uint32(0); i < count; i++ {
		pid := infos[i].Process.ProcessId
		if pid == 0 || pid == selfPID {
			continue
		}
		exe := queryProcExeBasename(pid)
		app := syscall.UTF16ToString(infos[i].AppName[:])
		label := ""
		if exe != "" && app != "" {
			el := strings.ToLower(exe)
			al := strings.ToLower(app)
			if el != al && el != al+".exe" {
				label = exe + " (" + app + ")"
			} else {
				label = exe
			}
		} else if exe != "" {
			label = exe
		} else {
			label = app
		}
		pushUniqueProc(out, seen, label)
	}
}

func winDetectBlockingProcesses(targetPath string) []string {
	var procs []string
	seen := make(map[string]struct{})
	if !dirExistsWin(targetPath) {
		return procs
	}
	collectRMBlockingProcesses(targetPath, &procs, seen)
	collectExplorerBlockingWindows(targetPath, &procs, seen)
	return procs
}

func formatOccupancyError(procs []string, moveErr error) string {
	if len(procs) > 0 {
		return "目录或其中文件正被以下程序占用，无法上锁：" + strings.Join(procs, "、") +
			"\n请先关闭相关文档或窗口后再试（注：记事本等打开后不持续占用的程序不影响上锁）。"
	}
	if moveErr == windows.ERROR_SHARING_VIOLATION || moveErr == windows.ERROR_ACCESS_DENIED {
		return "目录或其中文件正被其他程序占用（例如命令行工作目录停留在其中，或资源管理器预览窗格打开了文件），请先关闭相关程序或窗口后再试。"
	}
	if errno, ok := moveErr.(syscall.Errno); ok {
		return fmt.Sprintf("锁定目录失败 (Win32 错误码: %d)", uint32(errno))
	}
	return fmt.Sprintf("锁定目录失败: %v", moveErr)
}

func winRepairLockedEntryStub(entry *core.DirEntry) bool {
	if !entry.Locked {
		return false
	}
	if entry.InPlaceACL {
		if !dirExistsWin(entry.Path) {
			return false
		}
		if !winIsDirectoryLocked(entry.Path, entry.WinSID) {
			_ = applyDenyAce(entry.Path, kLockDenyMask, &entry.WinSID)
		}
		stub := entry.StubPath
		if stub == "" {
			stub = winMakeCompanionDlockPath(entry.Path)
		}
		if fileExistsWin(stub) {
			entry.StubPath = stub
			return false
		}
		if !createLockedDlockStub(stub, *entry) {
			return false
		}
		entry.StubPath = stub
		notifyShellParentUpdated(stub)
		return true
	}

	if entry.VaultPath == "" || !dirExistsWin(entry.VaultPath) {
		return false
	}
	if _, ok := getFileAttrsWin(entry.Path); ok {
		return false
	}
	dlockPath := entry.Path + ".dlock"
	if fileExistsWin(dlockPath) {
		entry.StubPath = dlockPath
		return false
	}
	if !createLockedDlockStub(dlockPath, *entry) {
		return false
	}
	entry.StubPath = dlockPath
	_ = os.Remove(entry.Path + ".lnk")
	notifyShellParentUpdated(entry.Path)
	return true
}

func WinSmartLock(entry *core.DirEntry, stateDir string, allowInPlaceFallback bool) error {
	origAttrs, ok := getFileAttrsWin(entry.Path)
	if !ok || (origAttrs&windows.FILE_ATTRIBUTE_DIRECTORY) == 0 {
		return fmt.Errorf("目标目录不存在或无法访问")
	}
	entry.WinOrigAttrs = origAttrs

	if !allowInPlaceFallback {
		if procs := winDetectBlockingProcesses(entry.Path); len(procs) > 0 {
			return fmt.Errorf("%s", formatOccupancyError(procs, windows.ERROR_SHARING_VIOLATION))
		}
	}

	dlockPath := entry.Path + ".dlock"
	if vaultPath, ok := chooseSameVolumeVault(entry.Path, stateDir, entry.ID); ok {
		vaultRoot := parentOfWinPath(vaultPath)
		if moveErr := moveDirWin(entry.Path, vaultPath); moveErr == nil {
			_ = setFileAttrsWin(vaultPath, windows.FILE_ATTRIBUTE_HIDDEN|windows.FILE_ATTRIBUTE_SYSTEM)
			_ = applyDenyAce(vaultPath, kLockDenyMask, &entry.WinSID)
			sealVaultRootIfNonEmpty(vaultRoot, false)

			if !createLockedDlockStub(dlockPath, *entry) {
				ensureHiddenDirUnlocked(vaultRoot)
				_ = removeDenyAce(vaultPath, kLockDenyMask, entry.WinSID)
				_ = setFileAttrsWin(vaultPath, origAttrs)
				_ = moveDirWin(vaultPath, entry.Path)
				sealVaultRootIfNonEmpty(vaultRoot, true)
				return fmt.Errorf("创建加密目录入口文件失败")
			}
			_ = os.Remove(entry.Path + ".lnk")
			deleteProtectedFile(winMakeCompanionDlockPath(entry.Path))

			entry.VaultPath = vaultPath
			entry.StubPath = dlockPath
			entry.InPlaceACL = false
			entry.Locked = true
			notifyShellParentUpdated(entry.Path)
			return nil
		} else {
			sealVaultRootIfNonEmpty(vaultRoot, true)
			if !allowInPlaceFallback {
				procs := winDetectBlockingProcesses(entry.Path)
				return fmt.Errorf("%s", formatOccupancyError(procs, moveErr))
			}
		}
	} else if !allowInPlaceFallback && !isUNCOrNonDriveWin(entry.Path) {
		return fmt.Errorf("无法为该路径创建本地保护区位置")
	}

	// In-place ACL fallback
	if err := applyDenyAce(entry.Path, kLockDenyMask, &entry.WinSID); err != nil {
		return err
	}
	companion := winMakeCompanionDlockPath(entry.Path)
	if createLockedDlockStub(companion, *entry) {
		entry.StubPath = companion
	} else {
		entry.StubPath = ""
	}
	entry.VaultPath = ""
	entry.InPlaceACL = true
	entry.Locked = true
	notifyShellParentUpdated(entry.Path)
	return nil
}

func WinSmartUnlock(entry *core.DirEntry) error {
	if entry.VaultPath != "" {
		if _, ok := getFileAttrsWin(entry.Path); ok {
			return fmt.Errorf("原路径已存在同名文件或目录，请先移走或重命名后再解锁（原数据在保护区中完好无损）")
		}
		vaultRoot := parentOfWinPath(entry.VaultPath)
		ensureHiddenDirUnlocked(vaultRoot)

		if _, ok := getFileAttrsWin(entry.VaultPath); !ok {
			sealVaultRootIfNonEmpty(vaultRoot, false)
			return fmt.Errorf("未在数据保护区中找到已锁定的目录")
		}
		_ = removeDenyAce(entry.VaultPath, kLockDenyMask, entry.WinSID)
		restoreAttrs := entry.WinOrigAttrs
		if restoreAttrs == 0 {
			restoreAttrs = windows.FILE_ATTRIBUTE_DIRECTORY
		}
		_ = setFileAttrsWin(entry.VaultPath, restoreAttrs & ^(uint32(windows.FILE_ATTRIBUTE_HIDDEN|windows.FILE_ATTRIBUTE_SYSTEM)))

		var lastErr error
		moved := false
		for attempt := 0; attempt < 4; attempt++ {
			if lastErr = moveDirWin(entry.VaultPath, entry.Path); lastErr == nil {
				moved = true
				break
			}
			time.Sleep(80 * time.Millisecond)
		}
		if !moved {
			_ = setFileAttrsWin(entry.VaultPath, windows.FILE_ATTRIBUTE_HIDDEN|windows.FILE_ATTRIBUTE_SYSTEM)
			_ = applyDenyAce(entry.VaultPath, kLockDenyMask, &entry.WinSID)
			sealVaultRootIfNonEmpty(vaultRoot, false)
			return fmt.Errorf("%s", formatWinErr("还原目录到原路径失败", lastErr))
		}

		_ = setFileAttrsWin(entry.Path, restoreAttrs)
		deleteProtectedFile(entry.Path + ".dlock")
		if entry.StubPath != "" && entry.StubPath != entry.Path+".dlock" {
			deleteProtectedFile(entry.StubPath)
		}
		deleteProtectedFile(winMakeCompanionDlockPath(entry.Path))
		_ = os.Remove(entry.Path + ".lnk")
		sealVaultRootIfNonEmpty(vaultRoot, true)
		notifyShellParentUpdated(entry.Path)

		entry.VaultPath = ""
		entry.StubPath = ""
		entry.InPlaceACL = false
		entry.Locked = false
		entry.LastUnlockedAt = core.NowEpoch()
		return nil
	}

	if err := removeDenyAce(entry.Path, kLockDenyMask, entry.WinSID); err != nil {
		return err
	}
	if entry.StubPath != "" {
		deleteProtectedFile(entry.StubPath)
		notifyShellParentUpdated(entry.StubPath)
	}
	deleteProtectedFile(winMakeCompanionDlockPath(entry.Path))
	deleteProtectedFile(entry.Path + ".dlock")

	entry.StubPath = ""
	entry.InPlaceACL = false
	entry.Locked = false
	entry.LastUnlockedAt = core.NowEpoch()
	notifyShellParentUpdated(entry.Path)
	return nil
}

func WinTryUpgradeInPlaceEntry(entry *core.DirEntry, stateDir string) bool {
	if !entry.Locked || !entry.InPlaceACL || isUNCOrNonDriveWin(entry.Path) {
		return false
	}
	if !isWinDriveReady(entry.Path) || !dirExistsWin(entry.Path) {
		return false
	}
	if len(winDetectBlockingProcesses(entry.Path)) > 0 {
		return false
	}
	vaultPath, ok := chooseSameVolumeVault(entry.Path, stateDir, entry.ID)
	if !ok {
		return false
	}
	if origAttrs, ok := getFileAttrsWin(entry.Path); ok {
		entry.WinOrigAttrs = origAttrs
	}
	vaultRoot := parentOfWinPath(vaultPath)

	_ = removeDenyAce(entry.Path, kLockDenyMask, entry.WinSID)
	if err := moveDirWin(entry.Path, vaultPath); err != nil {
		_ = applyDenyAce(entry.Path, kLockDenyMask, &entry.WinSID)
		sealVaultRootIfNonEmpty(vaultRoot, true)
		return false
	}
	_ = setFileAttrsWin(vaultPath, windows.FILE_ATTRIBUTE_HIDDEN|windows.FILE_ATTRIBUTE_SYSTEM)
	_ = applyDenyAce(vaultPath, kLockDenyMask, &entry.WinSID)
	sealVaultRootIfNonEmpty(vaultRoot, false)

	stdStub := entry.Path + ".dlock"
	if !createLockedDlockStub(stdStub, *entry) {
		ensureHiddenDirUnlocked(vaultRoot)
		_ = removeDenyAce(vaultPath, kLockDenyMask, entry.WinSID)
		restoreAttrs := entry.WinOrigAttrs
		if restoreAttrs == 0 {
			restoreAttrs = windows.FILE_ATTRIBUTE_DIRECTORY
		}
		_ = setFileAttrsWin(vaultPath, restoreAttrs)
		_ = moveDirWin(vaultPath, entry.Path)
		_ = applyDenyAce(entry.Path, kLockDenyMask, &entry.WinSID)
		sealVaultRootIfNonEmpty(vaultRoot, true)
		return false
	}

	if entry.StubPath != "" && !strings.EqualFold(entry.StubPath, stdStub) {
		deleteProtectedFile(entry.StubPath)
	}
	deleteProtectedFile(winMakeCompanionDlockPath(entry.Path))

	entry.VaultPath = vaultPath
	entry.StubPath = stdStub
	entry.InPlaceACL = false
	entry.Locked = true
	notifyShellParentUpdated(entry.Path)
	return true
}

func collectExistingVaultRoots(stateDir string) []string {
	var roots []string
	seen := make(map[string]struct{})
	add := func(cand string) {
		if cand == "" {
			return
		}
		key := strings.ToLower(core.NormalizeWinPath(cand))
		if _, exists := seen[key]; !exists && dirExistsWin(cand) {
			seen[key] = struct{}{}
			roots = append(roots, cand)
		}
	}
	if stateDir != "" {
		add(filepath.Join(stateDir, "Vault"))
	}
	mask, _, _ := procGetLogicalDrives.Call()
	for i := 0; i < 26; i++ {
		if (mask & (1 << uint(i))) == 0 {
			continue
		}
		drv := string(rune('A' + i))
		root := drv + `:\`
		wroot, _ := syscall.UTF16PtrFromString(root)
		dtype, _, _ := procGetDriveTypeW.Call(uintptr(unsafe.Pointer(wroot)))
		if dtype != driveFixed && dtype != driveRemovable {
			continue
		}
		add(drv + `:\$目录密匣_重要数据保护区_切勿删除`)
	}
	return roots
}

func listVaultSubdirsReadonly(vroot string) []string {
	var out []string
	if !dirExistsWin(vroot) {
		return out
	}
	hadACL := winIsDirectoryLocked(vroot, "S-1-1-0")
	if hadACL {
		_ = removeDenyAce(vroot, kLockDenyMask, "S-1-1-0")
	}
	if entries, err := os.ReadDir(vroot); err == nil {
		for _, ent := range entries {
			if ent.IsDir() {
				out = append(out, filepath.Join(vroot, ent.Name()))
			}
		}
	}
	if hadACL {
		sealVaultRootIfNonEmpty(vroot, false)
	}
	return out
}

func WinDiagnoseState(store *core.StateStore, stateDir string) *core.HealthReport {
	rep := &core.HealthReport{Items: []core.HealthItem{}}
	knownVaults := make(map[string]struct{})

	for _, entry := range store.State.Entries {
		rep.TotalEntries++
		if entry.VaultPath != "" {
			knownVaults[strings.ToLower(core.NormalizeWinPath(entry.VaultPath))] = struct{}{}
		}
		item := core.HealthItem{
			EntryID:     entry.ID,
			DisplayPath: entry.Path,
			VaultPath:   entry.VaultPath,
		}
		if !isWinDriveReady(entry.Path) || (entry.VaultPath != "" && !isWinDriveReady(entry.VaultPath)) {
			item.Kind = "unmounted_disk"
			item.Summary = "所在磁盘或移动驱动器当前未就绪"
			item.RepairHint = "若为U盘或移动硬盘，请接入后再扫描"
			rep.UnmountedCount++
			rep.Items = append(rep.Items, item)
			continue
		}

		origExists := dirExistsWin(entry.Path)
		vaultExists := entry.VaultPath != "" && dirExistsWin(entry.VaultPath)

		if entry.Locked && !entry.InPlaceACL {
			if origExists && vaultExists {
				item.Kind = "conflict_both_exist"
				item.Summary = "保护区与原路径同时存在同名目录（冲突）"
				item.RepairHint = "为保数据安全不会自动合并，请打开保护区手工核对"
				rep.ManualCount++
				rep.Items = append(rep.Items, item)
				continue
			}
			if vaultExists && !origExists {
				if !fileExistsWin(entry.Path + ".dlock") {
					item.Kind = "missing_stub"
					item.Summary = "保护区数据完好，但 .dlock 入口文件丢失"
					item.RepairHint = "可一键补建 .dlock 入口并校验保护区权限"
					item.AutoRepairable = true
					rep.RepairableCount++
					rep.Items = append(rep.Items, item)
				} else {
					rep.OKCount++
				}
				continue
			}
			if !vaultExists && origExists {
				if !winIsDirectoryLocked(entry.Path, entry.WinSID) {
					item.Kind = "externally_restored"
					item.Summary = "目录已在外部恢复为未锁定状态，记录仍标记已锁定"
					item.RepairHint = "可一键同步状态为“未锁定”并清理残留入口"
					item.AutoRepairable = true
					rep.RepairableCount++
				} else {
					item.Kind = "missing_companion_stub"
					item.Summary = "目录处于原位锁定状态，需要同步记录与伴随入口"
					item.RepairHint = "可一键补齐原位伴随 .dlock 入口"
					item.AutoRepairable = true
					rep.RepairableCount++
				}
				rep.Items = append(rep.Items, item)
				continue
			}
			item.Kind = "vault_missing"
			item.Summary = "原目录与保护区目录均不存在（可能被外部卸载或删除）"
			item.RepairHint = "核实数据不在磁盘后，可勾选“清理已失联记录”移出列表"
			item.AllowRemoveRecord = true
			rep.ManualCount++
			rep.Items = append(rep.Items, item)
			continue
		}

		if entry.Locked && entry.InPlaceACL {
			if !origExists {
				item.Kind = "vault_missing"
				item.Summary = "原位锁定目录已不存在于磁盘"
				item.RepairHint = "核实磁盘状态后可勾选“清理已失联记录”移出列表"
				item.AllowRemoveRecord = true
				rep.ManualCount++
				rep.Items = append(rep.Items, item)
				continue
			}
			if !winIsDirectoryLocked(entry.Path, entry.WinSID) {
				item.Kind = "externally_restored"
				item.Summary = "原位锁权限已被外部解除，但记录仍标记已锁定"
				item.RepairHint = "可一键同步为“未锁定”并清理伴随 .dlock"
				item.AutoRepairable = true
				rep.RepairableCount++
				rep.Items = append(rep.Items, item)
				continue
			}
			stub := entry.StubPath
			if stub == "" {
				stub = winMakeCompanionDlockPath(entry.Path)
			}
			if !fileExistsWin(stub) {
				item.Kind = "missing_companion_stub"
				item.Summary = "原位锁定目录正常，但伴随（点此解锁）.dlock 丢失"
				item.RepairHint = "可一键补建伴随 .dlock 解锁入口"
				item.AutoRepairable = true
				rep.RepairableCount++
				rep.Items = append(rep.Items, item)
			} else {
				rep.OKCount++
			}
			continue
		}

		hasStaleStub := fileExistsWin(entry.Path+".dlock") ||
			fileExistsWin(winMakeCompanionDlockPath(entry.Path)) ||
			(entry.StubPath != "" && fileExistsWin(entry.StubPath))
		if hasStaleStub {
			item.Kind = "stale_unlocked_stub"
			item.Summary = "目录已解锁，但旁边仍残留多余的 .dlock 入口文件"
			item.RepairHint = "可一键清理残留 .dlock 文件"
			item.AutoRepairable = true
			rep.RepairableCount++
			rep.Items = append(rep.Items, item)
			continue
		}
		if !origExists {
			item.Kind = "missing_unlocked_dir"
			item.Summary = "未锁定目录已被外部移动或删除"
			item.RepairHint = "若已不再需要该目录，可勾选“清理已失联记录”移出列表"
			item.AllowRemoveRecord = true
			rep.ManualCount++
			rep.Items = append(rep.Items, item)
			continue
		}
		rep.OKCount++
	}

	for _, vroot := range collectExistingVaultRoots(stateDir) {
		for _, sub := range listVaultSubdirsReadonly(vroot) {
			key := strings.ToLower(core.NormalizeWinPath(sub))
			if _, ok := knownVaults[key]; !ok {
				rep.OrphanVaultCount++
				rep.ManualCount++
				rep.Items = append(rep.Items, core.HealthItem{
					DisplayPath: sub,
					VaultPath:   sub,
					Kind:        "orphan_vault_dir",
					Summary:     "保护区中存在未绑定状态记录的目录（数据完好）",
					RepairHint:  "系统不会自动删除保护区数据，请按《应急恢复说明》手工核对",
				})
			}
		}
	}
	return rep
}

func WinRepairState(store *core.StateStore, stateDir string, cleanDeadRecords bool) (*core.RepairResult, error) {
	rep := WinDiagnoseState(store, stateDir)
	res := &core.RepairResult{Details: []string{}}
	changed := false
	idsToRemove := make(map[string]struct{})

	for _, item := range rep.Items {
		if item.EntryID == "" {
			continue
		}
		entry := store.FindByID(item.EntryID)
		if entry == nil {
			continue
		}
		switch item.Kind {
		case "missing_stub":
			if winRepairLockedEntryStub(entry) {
				_ = applyDenyAce(entry.VaultPath, kLockDenyMask, &entry.WinSID)
				changed = true
				res.RepairedCount++
				res.Details = append(res.Details, "已补建 .dlock 入口: "+entry.Path)
			}
		case "missing_companion_stub":
			if !entry.InPlaceACL && (entry.VaultPath == "" || !dirExistsWin(entry.VaultPath)) {
				entry.InPlaceACL = true
				entry.VaultPath = ""
			}
			if winRepairLockedEntryStub(entry) {
				changed = true
				res.RepairedCount++
				res.Details = append(res.Details, "已补建伴随解锁入口: "+entry.Path)
			}
		case "externally_restored":
			deleteProtectedFile(entry.Path + ".dlock")
			if entry.StubPath != "" {
				deleteProtectedFile(entry.StubPath)
			}
			deleteProtectedFile(winMakeCompanionDlockPath(entry.Path))
			entry.Locked = false
			entry.InPlaceACL = false
			entry.VaultPath = ""
			entry.StubPath = ""
			notifyShellParentUpdated(entry.Path)
			changed = true
			res.RepairedCount++
			res.Details = append(res.Details, "已同步外部恢复状态为未锁定: "+entry.Path)
		case "stale_unlocked_stub":
			deleteProtectedFile(entry.Path + ".dlock")
			if entry.StubPath != "" {
				deleteProtectedFile(entry.StubPath)
			}
			deleteProtectedFile(winMakeCompanionDlockPath(entry.Path))
			entry.StubPath = ""
			notifyShellParentUpdated(entry.Path)
			changed = true
			res.RepairedCount++
			res.Details = append(res.Details, "已清理残留 .dlock 文件: "+entry.Path)
		case "vault_missing", "missing_unlocked_dir":
			if cleanDeadRecords {
				deleteProtectedFile(entry.Path + ".dlock")
				if entry.StubPath != "" {
					deleteProtectedFile(entry.StubPath)
				}
				deleteProtectedFile(winMakeCompanionDlockPath(entry.Path))
				idsToRemove[entry.ID] = struct{}{}
				changed = true
				res.RepairedCount++
				res.Details = append(res.Details, "已移除失联记录: "+entry.Path)
			}
		}
	}

	if len(idsToRemove) > 0 {
		kept := make([]core.DirEntry, 0, len(store.State.Entries))
		for _, e := range store.State.Entries {
			if _, drop := idsToRemove[e.ID]; !drop {
				kept = append(kept, e)
			}
		}
		store.State.Entries = kept
	}
	if changed {
		if err := store.Save(); err != nil {
			return nil, err
		}
	}
	return res, nil
}
