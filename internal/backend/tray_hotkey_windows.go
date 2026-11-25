//go:build windows

package backend

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"unsafe"

	"dirlocker/internal/core"
)

const (
	wmDestroy     = 0x0002
	wmCommand     = 0x0111
	wmHotKey      = 0x0312
	wmApp         = 0x8000
	wmTrayIcon    = wmApp + 1
	wmSetHotkey   = wmApp + 2
	wmTestHotkey  = wmApp + 3
	wmQuitWindow  = wmApp + 4
	wmQuitSilent  = wmApp + 5
	wmLButtonUp   = 0x0202
	wmLButtonDbl  = 0x0203
	wmRButtonUp   = 0x0205
	wmContextMenu = 0x007B

	nimAdd        = 0x00000000
	nimDelete     = 0x00000002
	nifMessage    = 0x00000001
	nifIcon       = 0x00000002
	nifTip        = 0x00000004
	mfString      = 0x00000000
	mfSeparator   = 0x00000800
	tpmRightAlign = 0x0008
	tpmBottomAlgn = 0x0020
	tpmReturnCmd  = 0x0100

	imageIcon       = 1
	lrLoadFromFile  = 0x0010
	lrDefaultSize   = 0x0040
	idiApplication  = 32512
	modAlt          = 0x0001
	modControl      = 0x0002
	modShift        = 0x0004
	modNoRepeat     = 0x4000
	hotkeyIDMain    = 1001
	hotkeyIDProbe   = 1002
	menuIDShow      = 2001
	menuIDLockAll   = 2002
	menuIDQuit      = 2003
	swShowNormal    = 1
	dwmwaCornerPref = 33
	dwmwcpRound     = 2

	trayWindowClass = "DirLockerWailsTrayHostV3"
)

var (
	modUser32  = syscall.NewLazyDLL("user32.dll")
	modDwmApi  = syscall.NewLazyDLL("dwmapi.dll")
	modGdi32   = syscall.NewLazyDLL("gdi32.dll")
	modShell32T = syscall.NewLazyDLL("shell32.dll")
	modKern32T = syscall.NewLazyDLL("kernel32.dll")

	procRegisterClassExW     = modUser32.NewProc("RegisterClassExW")
	procCreateWindowExW      = modUser32.NewProc("CreateWindowExW")
	procDefWindowProcW       = modUser32.NewProc("DefWindowProcW")
	procDestroyWindow        = modUser32.NewProc("DestroyWindow")
	procGetMessageW          = modUser32.NewProc("GetMessageW")
	procTranslateMessage     = modUser32.NewProc("TranslateMessage")
	procDispatchMessageW     = modUser32.NewProc("DispatchMessageW")
	procPostMessageW         = modUser32.NewProc("PostMessageW")
	procPostQuitMessage      = modUser32.NewProc("PostQuitMessage")
	procFindWindowW          = modUser32.NewProc("FindWindowW")
	procRegisterHotKey       = modUser32.NewProc("RegisterHotKey")
	procUnregisterHotKey     = modUser32.NewProc("UnregisterHotKey")
	procGetLastInputInfo     = modUser32.NewProc("GetLastInputInfo")
	procCreatePopupMenu      = modUser32.NewProc("CreatePopupMenu")
	procAppendMenuW          = modUser32.NewProc("AppendMenuW")
	procTrackPopupMenuEx     = modUser32.NewProc("TrackPopupMenuEx")
	procDestroyMenu          = modUser32.NewProc("DestroyMenu")
	procGetCursorPos         = modUser32.NewProc("GetCursorPos")
	procSetForegroundWindow  = modUser32.NewProc("SetForegroundWindow")
	procLoadImageW           = modUser32.NewProc("LoadImageW")
	procLoadIconW            = modUser32.NewProc("LoadIconW")
	procDestroyIcon          = modUser32.NewProc("DestroyIcon")
	procEnumWindows          = modUser32.NewProc("EnumWindows")
	procGetWindowThreadPID   = modUser32.NewProc("GetWindowThreadProcessId")
	procGetClassNameW        = modUser32.NewProc("GetClassNameW")
	procIsZoomed             = modUser32.NewProc("IsZoomed")
	procGetWindowRect        = modUser32.NewProc("GetWindowRect")
	procSetWindowRgn         = modUser32.NewProc("SetWindowRgn")
	procDwmSetWindowAttr     = modDwmApi.NewProc("DwmSetWindowAttribute")
	procCreateRoundRectRgn   = modGdi32.NewProc("CreateRoundRectRgn")
	procShellNotifyIconW     = modShell32T.NewProc("Shell_NotifyIconW")
	procExtractIconExW       = modShell32T.NewProc("ExtractIconExW")
	procShellExecuteW        = modShell32T.NewProc("ShellExecuteW")
	procGetModuleHandleW     = modKern32T.NewProc("GetModuleHandleW")
	procGetTickCount64       = modKern32T.NewProc("GetTickCount64")
	procGetTickCount         = modKern32T.NewProc("GetTickCount")
)

type wndClassExW struct {
	Size       uint32
	Style      uint32
	WndProc    uintptr
	ClsExtra   int32
	WndExtra   int32
	Instance   uintptr
	Icon       uintptr
	Cursor     uintptr
	Background uintptr
	MenuName   *uint16
	ClassName  *uint16
	IconSm     uintptr
}

type winMsg struct {
	Hwnd    uintptr
	Message uint32
	WParam  uintptr
	LParam  uintptr
	Time    uint32
	Pt      winPoint
}

type winPoint struct {
	X int32
	Y int32
}

type winRect struct {
	Left   int32
	Top    int32
	Right  int32
	Bottom int32
}

type lastInputInfo struct {
	CbSize uint32
	DwTime uint32
}

type notifyIconDataW struct {
	CbSize           uint32
	HWnd             uintptr
	UID              uint32
	UFlags           uint32
	UCallbackMessage uint32
	HIcon            uintptr
	SzTip            [128]uint16
	DwState          uint32
	DwStateMask      uint32
	SzInfo           [256]uint16
	UVersion         uint32
	SzInfoTitle      [64]uint16
	DwInfoFlags      uint32
	GuidItem         [16]byte
	HBalloonIcon     uintptr
}

type hotkeyReq struct {
	spec core.HotkeySpec
	done chan struct {
		ok  bool
		msg string
	}
}

type windowsPlatformHooks struct {
	mu           sync.Mutex
	hwnd         uintptr
	hIcon        uintptr
	trayAdded    bool
	activeSpec   core.HotkeySpec
	hotkeyActive bool
	onShow       func()
	onLockAll    func()
	onQuit       func()
	pendingReq   *hotkeyReq
	readyCh      chan struct{}
	stopOnce     sync.Once
}

var activeWinHooks *windowsPlatformHooks

func NewPlatformHooks() PlatformHooks {
	return &windowsPlatformHooks{
		readyCh: make(chan struct{}),
	}
}

// SendQuitSilentToRunningInstance signals any running v2.x/v3.x DirLocker tray window to exit cleanly.
func SendQuitSilentToRunningInstance() bool {
	// v3.x Wails host window
	wcls3, _ := syscall.UTF16PtrFromString(trayWindowClass)
	if hwnd, _, _ := procFindWindowW.Call(uintptr(unsafe.Pointer(wcls3)), 0); hwnd != 0 {
		procPostMessageW.Call(hwnd, wmQuitSilent, 0, 0)
		return true
	}
	// Legacy v1.x/v2.x Win32 window class ("DirLockerMainWindow")
	wclsLegacy, _ := syscall.UTF16PtrFromString("DirLockerMainWindow")
	if hwnd, _, _ := procFindWindowW.Call(uintptr(unsafe.Pointer(wclsLegacy)), 0); hwnd != 0 {
		procPostMessageW.Call(hwnd, wmDestroy, 0, 0)
		return true
	}
	return false
}

func hotkeySpecToWin(spec core.HotkeySpec) (mods uint32, vk uint32, ok bool) {
	if !spec.IsValid() {
		return 0, 0, false
	}
	if spec.Ctrl {
		mods |= modControl
	}
	if spec.Alt {
		mods |= modAlt
	}
	if spec.Shift {
		mods |= modShift
	}
	mods |= modNoRepeat
	if len(spec.Key) == 1 {
		c := spec.Key[0]
		if (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') {
			return mods, uint32(c), true
		}
	}
	if len(spec.Key) >= 2 && spec.Key[0] == 'F' {
		var fn int
		if _, err := fmt.Sscanf(spec.Key, "F%d", &fn); err == nil && fn >= 1 && fn <= 12 {
			return mods, uint32(0x70 + fn - 1), true
		}
	}
	return 0, 0, false
}

func loadTrayIcon() uintptr {
	exe := getSelfExePath()
	if exe != "" {
		icoPath := filepath.Join(filepath.Dir(exe), "app.ico")
		if fileExistsWin(icoPath) {
			wp, _ := syscall.UTF16PtrFromString(icoPath)
			h, _, _ := procLoadImageW.Call(0, uintptr(unsafe.Pointer(wp)), imageIcon, 32, 32, lrLoadFromFile|lrDefaultSize)
			if h != 0 {
				return h
			}
		}
		wexe, _ := syscall.UTF16PtrFromString(exe)
		var smallIcon uintptr
		r, _, _ := procExtractIconExW.Call(uintptr(unsafe.Pointer(wexe)), 0, 0, uintptr(unsafe.Pointer(&smallIcon)), 1)
		if r > 0 && smallIcon != 0 {
			return smallIcon
		}
	}
	h, _, _ := procLoadIconW.Call(0, idiApplication)
	return h
}

func (h *windowsPlatformHooks) Start(onShow func(), onLockAll func(), onQuit func()) {
	h.mu.Lock()
	h.onShow = onShow
	h.onLockAll = onLockAll
	h.onQuit = onQuit
	activeWinHooks = h
	h.mu.Unlock()

	go h.runMessageLoop()
	<-h.readyCh
}

func (h *windowsPlatformHooks) runMessageLoop() {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	hInst, _, _ := procGetModuleHandleW.Call(0)
	clsName, _ := syscall.UTF16PtrFromString(trayWindowClass)
	wndProc := syscall.NewCallback(winTrayWndProc)

	wc := wndClassExW{
		Size:      uint32(unsafe.Sizeof(wndClassExW{})),
		WndProc:   wndProc,
		Instance:  hInst,
		ClassName: clsName,
	}
	procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))

	hwnd, _, _ := procCreateWindowExW.Call(
		0,
		uintptr(unsafe.Pointer(clsName)),
		uintptr(unsafe.Pointer(clsName)),
		0, 0, 0, 0, 0, 0, 0, hInst, 0,
	)

	h.mu.Lock()
	h.hwnd = hwnd
	h.hIcon = loadTrayIcon()
	h.mu.Unlock()

	if hwnd != 0 {
		h.addTrayIcon(hwnd)
	}
	close(h.readyCh)

	var msg winMsg
	for {
		ret, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
		if int32(ret) <= 0 {
			break
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&msg)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
	}

	if hwnd != 0 {
		procUnregisterHotKey.Call(hwnd, hotkeyIDMain)
		h.removeTrayIcon(hwnd)
	}
	if h.hIcon != 0 {
		procDestroyIcon.Call(h.hIcon)
	}
}

func (h *windowsPlatformHooks) addTrayIcon(hwnd uintptr) {
	var nid notifyIconDataW
	nid.CbSize = uint32(unsafe.Sizeof(nid))
	nid.HWnd = hwnd
	nid.UID = 1
	nid.UFlags = nifMessage | nifIcon | nifTip
	nid.UCallbackMessage = wmTrayIcon
	nid.HIcon = h.hIcon
	tip, _ := syscall.UTF16FromString("目录密匣 (DirLocker)")
	copy(nid.SzTip[:], tip)

	ok, _, _ := procShellNotifyIconW.Call(nimAdd, uintptr(unsafe.Pointer(&nid)))
	h.mu.Lock()
	h.trayAdded = ok != 0
	h.mu.Unlock()
}

func (h *windowsPlatformHooks) removeTrayIcon(hwnd uintptr) {
	h.mu.Lock()
	added := h.trayAdded
	h.trayAdded = false
	h.mu.Unlock()
	if !added {
		return
	}
	var nid notifyIconDataW
	nid.CbSize = uint32(unsafe.Sizeof(nid))
	nid.HWnd = hwnd
	nid.UID = 1
	procShellNotifyIconW.Call(nimDelete, uintptr(unsafe.Pointer(&nid)))
}

func (h *windowsPlatformHooks) showTrayContextMenu(hwnd uintptr) {
	menu, _, _ := procCreatePopupMenu.Call()
	if menu == 0 {
		return
	}
	defer procDestroyMenu.Call(menu)

	tShow, _ := syscall.UTF16PtrFromString("打开 目录密匣")
	tLock, _ := syscall.UTF16PtrFromString("一键锁定所有目录")
	tQuit, _ := syscall.UTF16PtrFromString("退出 目录密匣")
	procAppendMenuW.Call(menu, mfString, menuIDShow, uintptr(unsafe.Pointer(tShow)))
	procAppendMenuW.Call(menu, mfString, menuIDLockAll, uintptr(unsafe.Pointer(tLock)))
	procAppendMenuW.Call(menu, mfSeparator, 0, 0)
	procAppendMenuW.Call(menu, mfString, menuIDQuit, uintptr(unsafe.Pointer(tQuit)))

	var pt winPoint
	procGetCursorPos.Call(uintptr(unsafe.Pointer(&pt)))
	procSetForegroundWindow.Call(hwnd)
	cmd, _, _ := procTrackPopupMenuEx.Call(menu, tpmRightAlign|tpmBottomAlgn|tpmReturnCmd, uintptr(pt.X), uintptr(pt.Y), hwnd, 0)
	procPostMessageW.Call(hwnd, 0, 0, 0)

	switch cmd {
	case menuIDShow:
		if h.onShow != nil {
			go h.onShow()
		}
	case menuIDLockAll:
		if h.onLockAll != nil {
			go h.onLockAll()
		}
	case menuIDQuit:
		if h.onQuit != nil {
			go h.onQuit()
		}
	}
}

func winTrayWndProc(hwnd uintptr, msg uint32, wparam, lparam uintptr) uintptr {
	h := activeWinHooks
	if h == nil {
		r, _, _ := procDefWindowProcW.Call(hwnd, uintptr(msg), wparam, lparam)
		return r
	}
	switch msg {
	case wmTrayIcon:
		ev := uint32(lparam & 0xFFFF)
		switch ev {
		case wmLButtonUp, wmLButtonDbl:
			if h.onShow != nil {
				go h.onShow()
			}
		case wmRButtonUp, wmContextMenu:
			h.showTrayContextMenu(hwnd)
		}
		return 0
	case wmHotKey:
		if wparam == hotkeyIDMain && h.onLockAll != nil {
			go h.onLockAll()
		}
		return 0
	case wmSetHotkey:
		h.mu.Lock()
		req := h.pendingReq
		h.pendingReq = nil
		h.mu.Unlock()
		if req != nil {
			procUnregisterHotKey.Call(hwnd, hotkeyIDMain)
			mods, vk, ok := hotkeySpecToWin(req.spec)
			if ok {
				r, _, _ := procRegisterHotKey.Call(hwnd, hotkeyIDMain, uintptr(mods), uintptr(vk))
				if r != 0 {
					h.mu.Lock()
					h.activeSpec = req.spec
					h.hotkeyActive = true
					h.mu.Unlock()
					req.done <- struct {
						ok  bool
						msg string
					}{true, ""}
					return 0
				}
			}
			h.mu.Lock()
			h.hotkeyActive = false
			h.mu.Unlock()
			req.done <- struct {
				ok  bool
				msg string
			}{false, fmt.Sprintf("全局快捷键 %s 注册失败，可能已被其他程序占用", req.spec.String())}
		}
		return 0
	case wmTestHotkey:
		h.mu.Lock()
		req := h.pendingReq
		h.pendingReq = nil
		curSpec := h.activeSpec
		curActive := h.hotkeyActive
		h.mu.Unlock()
		if req != nil {
			if curActive && curSpec.String() == req.spec.String() {
				req.done <- struct {
					ok  bool
					msg string
				}{true, "当前快捷键已生效，无冲突"}
				return 0
			}
			mods, vk, ok := hotkeySpecToWin(req.spec)
			if !ok {
				req.done <- struct {
					ok  bool
					msg string
				}{false, "快捷键格式不支持"}
				return 0
			}
			r, _, _ := procRegisterHotKey.Call(hwnd, hotkeyIDProbe, uintptr(mods), uintptr(vk))
			if r != 0 {
				procUnregisterHotKey.Call(hwnd, hotkeyIDProbe)
				req.done <- struct {
					ok  bool
					msg string
				}{true, fmt.Sprintf("快捷键 %s 可用，未发现系统冲突", req.spec.String())}
			} else {
				req.done <- struct {
					ok  bool
					msg string
				}{false, fmt.Sprintf("快捷键 %s 已被系统或其他程序占用，请更换组合", req.spec.String())}
			}
		}
		return 0
	case wmQuitSilent:
		os.Exit(0)
		return 0
	case wmQuitWindow:
		procDestroyWindow.Call(hwnd)
		return 0
	case wmDestroy:
		procPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := procDefWindowProcW.Call(hwnd, uintptr(msg), wparam, lparam)
	return r
}

func (h *windowsPlatformHooks) Stop() {
	h.stopOnce.Do(func() {
		h.mu.Lock()
		hwnd := h.hwnd
		h.mu.Unlock()
		if hwnd != 0 {
			procPostMessageW.Call(hwnd, wmQuitWindow, 0, 0)
		}
	})
}

func (h *windowsPlatformHooks) RegisterHotkey(hotkey string) (bool, string) {
	spec, err := core.ParseHotkey(hotkey)
	if err != nil {
		return false, err.Error()
	}
	h.mu.Lock()
	hwnd := h.hwnd
	if hwnd == 0 {
		h.mu.Unlock()
		return false, "后台消息窗口未就绪"
	}
	req := &hotkeyReq{
		spec: spec,
		done: make(chan struct {
			ok  bool
			msg string
		}, 1),
	}
	h.pendingReq = req
	h.mu.Unlock()

	procPostMessageW.Call(hwnd, wmSetHotkey, 0, 0)
	res := <-req.done
	return res.ok, res.msg
}

func (h *windowsPlatformHooks) TestHotkey(hotkey string) (bool, string) {
	spec, err := core.ParseHotkey(hotkey)
	if err != nil {
		return false, err.Error()
	}
	h.mu.Lock()
	hwnd := h.hwnd
	if hwnd == 0 {
		h.mu.Unlock()
		return false, "后台消息窗口未就绪"
	}
	req := &hotkeyReq{
		spec: spec,
		done: make(chan struct {
			ok  bool
			msg string
		}, 1),
	}
	h.pendingReq = req
	h.mu.Unlock()

	procPostMessageW.Call(hwnd, wmTestHotkey, 0, 0)
	res := <-req.done
	return res.ok, res.msg
}

func (h *windowsPlatformHooks) IdleSeconds() (int, bool) {
	var lii lastInputInfo
	lii.CbSize = uint32(unsafe.Sizeof(lii))
	ok, _, _ := procGetLastInputInfo.Call(uintptr(unsafe.Pointer(&lii)))
	if ok == 0 {
		return 0, false
	}
	tick, _, _ := procGetTickCount.Call()
	elapsedMs := uint32(tick) - lii.DwTime
	return int(elapsedMs / 1000), true
}

func (h *windowsPlatformHooks) TrayAvailable() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.trayAdded
}

func (h *windowsPlatformHooks) ApplyWindowRoundedCorners() {
	pid := uint32(os.Getpid())
	var targetHwnd uintptr
	cb := syscall.NewCallback(func(hwnd uintptr, lparam uintptr) uintptr {
		var winPid uint32
		procGetWindowThreadPID.Call(hwnd, uintptr(unsafe.Pointer(&winPid)))
		if winPid != pid {
			return 1
		}
		var clsBuf [64]uint16
		n, _, _ := procGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&clsBuf[0])), uintptr(len(clsBuf)))
		if n > 0 && syscall.UTF16ToString(clsBuf[:n]) == "wailsWindow" {
			targetHwnd = hwnd
			return 0
		}
		return 1
	})
	procEnumWindows.Call(cb, 0)
	if targetHwnd == 0 {
		return
	}

	pref := uint32(dwmwcpRound)
	hr, _, _ := procDwmSetWindowAttr.Call(
		targetHwnd,
		dwmwaCornerPref,
		uintptr(unsafe.Pointer(&pref)),
		unsafe.Sizeof(pref),
	)
	if hr == 0 {
		return
	}

	zoomed, _, _ := procIsZoomed.Call(targetHwnd)
	if zoomed != 0 {
		procSetWindowRgn.Call(targetHwnd, 0, 1)
		return
	}
	var r winRect
	procGetWindowRect.Call(targetHwnd, uintptr(unsafe.Pointer(&r)))
	w := r.Right - r.Left
	hgt := r.Bottom - r.Top
	if w > 0 && hgt > 0 {
		hrgn, _, _ := procCreateRoundRectRgn.Call(0, 0, uintptr(w+1), uintptr(hgt+1), 20, 20)
		if hrgn != 0 {
			procSetWindowRgn.Call(targetHwnd, hrgn, 1)
		}
	}
}

func (h *windowsPlatformHooks) OpenURL(u string) error {
	return shellExecuteOpen(u)
}

func shellExecuteOpen(target string) error {
	verbPtr, err := syscall.UTF16PtrFromString("open")
	if err != nil {
		return err
	}
	targetPtr, err := syscall.UTF16PtrFromString(target)
	if err != nil {
		return err
	}
	ret, _, callErr := procShellExecuteW.Call(0, uintptr(unsafe.Pointer(verbPtr)), uintptr(unsafe.Pointer(targetPtr)), 0, 0, swShowNormal)
	if ret <= 32 {
		if callErr != nil && callErr != syscall.Errno(0) {
			return fmt.Errorf("打开失败: %w", callErr)
		}
		return fmt.Errorf("打开失败 (code: %d)", ret)
	}
	return nil
}
