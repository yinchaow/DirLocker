//go:build linux

package backend

/*
#cgo LDFLAGS: -lX11 -ldl
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

// Initialize Xlib thread safety before GTK/GDK initializes X11.
__attribute__((constructor))
static void dl_init_x11_threads(void) {
    XInitThreads();
}

static int dl_grab_error = 0;
static int dl_x_err_handler(Display* dpy, XErrorEvent* ev) {
    (void)dpy;
    if (ev && (ev->error_code == BadAccess || ev->error_code == BadValue)) {
        dl_grab_error = 1;
    }
    return 0;
}

static int dl_grab_combo(Display* dpy, KeyCode code, unsigned int base_mods) {
    if (!dpy || code == 0) return 0;
    Window root = DefaultRootWindow(dpy);
    unsigned int masks[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    dl_grab_error = 0;
    int (*old_handler)(Display*, XErrorEvent*) = XSetErrorHandler(dl_x_err_handler);
    for (int i = 0; i < 4; ++i) {
        XGrabKey(dpy, code, base_mods | masks[i], root, False, GrabModeAsync, GrabModeAsync);
    }
    XSync(dpy, False);
    XSetErrorHandler(old_handler);
    if (dl_grab_error) {
        for (int i = 0; i < 4; ++i) {
            XUngrabKey(dpy, code, base_mods | masks[i], root);
        }
        XSync(dpy, False);
        return 0;
    }
    return 1;
}

static void dl_ungrab_combo(Display* dpy, KeyCode code, unsigned int base_mods) {
    if (!dpy || code == 0) return;
    Window root = DefaultRootWindow(dpy);
    unsigned int masks[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (int i = 0; i < 4; ++i) {
        XUngrabKey(dpy, code, base_mods | masks[i], root);
    }
    XSync(dpy, False);
}

static int dl_poll_keypress(Display* dpy, KeyCode* out_code, unsigned int* out_state) {
    if (!dpy) return 0;
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress) {
            *out_code = (KeyCode)ev.xkey.keycode;
            *out_state = ev.xkey.state & (ControlMask | Mod1Mask | ShiftMask);
            return 1;
        }
    }
    return 0;
}

typedef struct {
    Window window;
    int state;
    int kind;
    unsigned long til_or_since;
    unsigned long idle;
    unsigned long eventMask;
} DL_XScreenSaverInfo;

typedef DL_XScreenSaverInfo* (*fn_xss_alloc)(void);
typedef Status (*fn_xss_query)(Display*, Drawable, DL_XScreenSaverInfo*);

static int dl_xss_idle_ms_on_display(Display* dpy, unsigned long* out_ms) {
    static void* handle = NULL;
    static fn_xss_alloc p_alloc = NULL;
    static fn_xss_query p_query = NULL;
    static int inited = 0;
    if (!dpy) return 0;
    if (!inited) {
        inited = 1;
        handle = dlopen("libXss.so.1", RTLD_LAZY);
        if (handle) {
            p_alloc = (fn_xss_alloc)dlsym(handle, "XScreenSaverAllocInfo");
            p_query = (fn_xss_query)dlsym(handle, "XScreenSaverQueryInfo");
        }
    }
    if (!p_alloc || !p_query) return 0;
    DL_XScreenSaverInfo* info = p_alloc();
    if (!info) return 0;
    int ok = 0;
    if (p_query(dpy, DefaultRootWindow(dpy), info) != 0) {
        *out_ms = info->idle;
        ok = 1;
    }
    XFree(info);
    return ok;
}
*/
import "C"

import (
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"sync"
	"time"
	"unsafe"

	"dirlocker/internal/core"
	"github.com/godbus/dbus/v5"
	"github.com/godbus/dbus/v5/introspect"
	"github.com/godbus/dbus/v5/prop"
)

type x11OpKind int

const (
	x11OpRegister x11OpKind = iota
	x11OpTest
	x11OpIdle
)

type x11Op struct {
	kind x11OpKind
	spec core.HotkeySpec
	done chan x11Res
}

type x11Res struct {
	ok   bool
	msg  string
	idle int
}

type linuxPlatformHooks struct {
	mu         sync.Mutex
	onShow     func()
	onLockAll  func()
	onQuit     func()
	stopCh     chan struct{}
	stopOnce   sync.Once
	x11ReqCh   chan x11Op
	x11Ready   bool
	dbusConn   *dbus.Conn
	trayActive bool
}

func NewPlatformHooks() PlatformHooks {
	return &linuxPlatformHooks{
		stopCh:   make(chan struct{}),
		x11ReqCh: make(chan x11Op, 8),
	}
}

func hotkeySpecToX11(dpy *C.Display, spec core.HotkeySpec) (C.KeyCode, C.uint, bool) {
	if dpy == nil || !spec.IsValid() {
		return 0, 0, false
	}
	var mods C.uint
	if spec.Ctrl {
		mods |= C.ControlMask
	}
	if spec.Alt {
		mods |= C.Mod1Mask
	}
	if spec.Shift {
		mods |= C.ShiftMask
	}
	ksName := spec.Key
	if len(ksName) == 1 && ksName[0] >= 'A' && ksName[0] <= 'Z' {
		ksName = strings.ToLower(ksName)
	}
	cs := C.CString(ksName)
	defer C.free(unsafe.Pointer(cs))
	sym := C.XStringToKeysym(cs)
	if sym == C.NoSymbol {
		return 0, 0, false
	}
	code := C.XKeysymToKeycode(dpy, sym)
	if code == 0 {
		return 0, 0, false
	}
	return code, mods, true
}

func (h *linuxPlatformHooks) Start(onShow func(), onLockAll func(), onQuit func()) {
	h.mu.Lock()
	h.onShow = onShow
	h.onLockAll = onLockAll
	h.onQuit = onQuit
	h.mu.Unlock()

	if os.Getenv("DISPLAY") != "" {
		readyCh := make(chan bool, 1)
		go h.runX11Worker(readyCh)
		ok := <-readyCh
		h.mu.Lock()
		h.x11Ready = ok
		h.mu.Unlock()
	}
	go h.startSNITray()
}

// runX11Worker owns a dedicated X11 Display* on a single locked OS thread so
// Xlib is never called concurrently across threads.
func (h *linuxPlatformHooks) runX11Worker(readyCh chan<- bool) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	defer func() { _ = recover() }()

	dpy := C.XOpenDisplay(nil)
	if dpy == nil {
		readyCh <- false
		return
	}
	defer C.XCloseDisplay(dpy)
	readyCh <- true

	var grabbedCode C.KeyCode
	var grabbedMods C.uint
	var activeSpec core.HotkeySpec
	hotkeyActive := false

	ticker := time.NewTicker(40 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-h.stopCh:
			if hotkeyActive {
				C.dl_ungrab_combo(dpy, grabbedCode, grabbedMods)
			}
			return

		case req := <-h.x11ReqCh:
			switch req.kind {
			case x11OpRegister:
				if hotkeyActive {
					C.dl_ungrab_combo(dpy, grabbedCode, grabbedMods)
					hotkeyActive = false
				}
				code, mods, ok := hotkeySpecToX11(dpy, req.spec)
				if !ok {
					req.done <- x11Res{ok: false, msg: "无法映射快捷键按键: " + req.spec.String()}
					continue
				}
				if C.dl_grab_combo(dpy, code, mods) == 0 {
					req.done <- x11Res{
						ok:  false,
						msg: fmt.Sprintf("全局快捷键 %s 注册失败，可能已被桌面环境或其他程序占用", req.spec.String()),
					}
					continue
				}
				grabbedCode = code
				grabbedMods = mods
				activeSpec = req.spec
				hotkeyActive = true
				req.done <- x11Res{ok: true}

			case x11OpTest:
				if hotkeyActive && activeSpec.String() == req.spec.String() {
					req.done <- x11Res{ok: true, msg: "当前快捷键已生效，无冲突"}
					continue
				}
				code, mods, ok := hotkeySpecToX11(dpy, req.spec)
				if !ok {
					req.done <- x11Res{ok: false, msg: "不支持该按键组合"}
					continue
				}
				if C.dl_grab_combo(dpy, code, mods) == 0 {
					req.done <- x11Res{
						ok:  false,
						msg: fmt.Sprintf("快捷键 %s 已被系统或其他程序占用，请更换组合", req.spec.String()),
					}
					continue
				}
				C.dl_ungrab_combo(dpy, code, mods)
				req.done <- x11Res{
					ok:  true,
					msg: fmt.Sprintf("快捷键 %s 可用，未发现系统冲突", req.spec.String()),
				}

			case x11OpIdle:
				var ms C.ulong
				if C.dl_xss_idle_ms_on_display(dpy, &ms) != 0 {
					req.done <- x11Res{ok: true, idle: int(ms / 1000)}
				} else {
					req.done <- x11Res{ok: false}
				}
			}

		case <-ticker.C:
			var gotCode C.KeyCode
			var gotState C.uint
			fired := false
			for C.dl_poll_keypress(dpy, &gotCode, &gotState) != 0 {
				if hotkeyActive && gotCode == grabbedCode && gotState == grabbedMods {
					fired = true
				}
			}
			if fired {
				h.mu.Lock()
				cb := h.onLockAll
				h.mu.Unlock()
				if cb != nil {
					go cb()
				}
			}
		}
	}
}

func (h *linuxPlatformHooks) sendX11Op(op x11Op) (x11Res, bool) {
	h.mu.Lock()
	ready := h.x11Ready
	h.mu.Unlock()
	if !ready {
		return x11Res{}, false
	}
	op.done = make(chan x11Res, 1)
	select {
	case h.x11ReqCh <- op:
	case <-h.stopCh:
		return x11Res{}, false
	case <-time.After(2 * time.Second):
		return x11Res{}, false
	}
	select {
	case res := <-op.done:
		return res, true
	case <-h.stopCh:
		return x11Res{}, false
	case <-time.After(2 * time.Second):
		return x11Res{}, false
	}
}

func (h *linuxPlatformHooks) Stop() {
	h.stopOnce.Do(func() {
		close(h.stopCh)
		h.mu.Lock()
		if h.dbusConn != nil {
			_ = h.dbusConn.Close()
			h.dbusConn = nil
		}
		h.trayActive = false
		h.x11Ready = false
		h.mu.Unlock()
	})
}

func (h *linuxPlatformHooks) RegisterHotkey(hotkey string) (bool, string) {
	spec, err := core.ParseHotkey(hotkey)
	if err != nil {
		return false, err.Error()
	}
	res, ok := h.sendX11Op(x11Op{kind: x11OpRegister, spec: spec})
	if !ok {
		return false, "当前桌面会话未提供 X11 显示连接，全局快捷键暂不可用"
	}
	return res.ok, res.msg
}

func (h *linuxPlatformHooks) TestHotkey(hotkey string) (bool, string) {
	spec, err := core.ParseHotkey(hotkey)
	if err != nil {
		return false, err.Error()
	}
	res, ok := h.sendX11Op(x11Op{kind: x11OpTest, spec: spec})
	if !ok {
		return false, "当前会话未检测到 X11 显示连接"
	}
	return res.ok, res.msg
}

func (h *linuxPlatformHooks) IdleSeconds() (int, bool) {
	// 1. Try XScreenSaver extension via dedicated X11 worker (works natively on Kylin UKUI / Xorg)
	if res, ok := h.sendX11Op(x11Op{kind: x11OpIdle}); ok && res.ok {
		return res.idle, true
	}
	// 2. Fallback to D-Bus ScreenSaver / Mutter IdleMonitor
	conn, err := dbus.SessionBus()
	if err != nil {
		return 0, false
	}
	var idleMs uint64
	if err := conn.Object("org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core").
		Call("org.gnome.Mutter.IdleMonitor.GetIdletime", 0).Store(&idleMs); err == nil {
		return int(idleMs / 1000), true
	}
	var idleSec uint32
	if err := conn.Object("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver").
		Call("org.freedesktop.ScreenSaver.GetSessionIdleTime", 0).Store(&idleSec); err == nil {
		return int(idleSec), true
	}
	return 0, false
}

func (h *linuxPlatformHooks) TrayAvailable() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.trayActive
}

func (h *linuxPlatformHooks) ApplyWindowRoundedCorners() {}

func (h *linuxPlatformHooks) OpenURL(u string) error {
	return exec.Command("xdg-open", u).Start()
}

// --- Native D-Bus StatusNotifierItem + DBusMenu (Kylin UKUI / KDE / AppIndicator tray) ---

// sniPixmap matches the StatusNotifierItem D-Bus spec signature (iiay).
type sniPixmap struct {
	Width  int32
	Height int32
	Data   []byte
}

// sniToolTip matches the StatusNotifierItem D-Bus spec signature (sa(iiay)ss).
type sniToolTip struct {
	IconName    string
	IconPixmap  []sniPixmap
	Title       string
	Description string
}

type sniItem struct {
	hooks *linuxPlatformHooks
}

func (s *sniItem) Activate(x, y int32) *dbus.Error {
	if s.hooks.onShow != nil {
		go s.hooks.onShow()
	}
	return nil
}

func (s *sniItem) SecondaryActivate(x, y int32) *dbus.Error {
	return s.Activate(x, y)
}

func (s *sniItem) ContextMenu(x, y int32) *dbus.Error {
	return nil
}

func (s *sniItem) Scroll(delta int32, orientation string) *dbus.Error {
	return nil
}

type dbusMenuLayout struct {
	ID         int32
	Properties map[string]dbus.Variant
	Children   []dbus.Variant
}

type sniMenu struct {
	hooks *linuxPlatformHooks
}

func (m *sniMenu) buildLayout() dbusMenuLayout {
	mkItem := func(id int32, label string) dbus.Variant {
		return dbus.MakeVariant(dbusMenuLayout{
			ID: id,
			Properties: map[string]dbus.Variant{
				"label":   dbus.MakeVariant(label),
				"enabled": dbus.MakeVariant(true),
				"visible": dbus.MakeVariant(true),
			},
			Children: []dbus.Variant{},
		})
	}
	sep := dbus.MakeVariant(dbusMenuLayout{
		ID: 100,
		Properties: map[string]dbus.Variant{
			"type":    dbus.MakeVariant("separator"),
			"visible": dbus.MakeVariant(true),
		},
		Children: []dbus.Variant{},
	})
	return dbusMenuLayout{
		ID: 0,
		Properties: map[string]dbus.Variant{
			"children-display": dbus.MakeVariant("submenu"),
		},
		Children: []dbus.Variant{
			mkItem(1, "打开 目录密匣"),
			mkItem(2, "一键锁定所有目录"),
			sep,
			mkItem(3, "退出 目录密匣"),
		},
	}
}

func (m *sniMenu) GetLayout(parentID int32, recursionDepth int32, propertyNames []string) (uint32, dbusMenuLayout, *dbus.Error) {
	return 1, m.buildLayout(), nil
}

func (m *sniMenu) GetGroupProperties(ids []int32, propertyNames []string) ([]struct {
	ID    int32
	Props map[string]dbus.Variant
}, *dbus.Error) {
	return []struct {
		ID    int32
		Props map[string]dbus.Variant
	}{}, nil
}

func (m *sniMenu) GetProperty(id int32, name string) (dbus.Variant, *dbus.Error) {
	return dbus.MakeVariant(""), nil
}

func (m *sniMenu) Event(id int32, eventID string, data dbus.Variant, timestamp uint32) *dbus.Error {
	if eventID != "clicked" {
		return nil
	}
	switch id {
	case 1:
		if m.hooks.onShow != nil {
			go m.hooks.onShow()
		}
	case 2:
		if m.hooks.onLockAll != nil {
			go m.hooks.onLockAll()
		}
	case 3:
		if m.hooks.onQuit != nil {
			go m.hooks.onQuit()
		}
	}
	return nil
}

func (m *sniMenu) EventGroup(events []struct {
	ID        int32
	EventID   string
	Data      dbus.Variant
	Timestamp uint32
}) ([]int32, *dbus.Error) {
	for _, ev := range events {
		_ = m.Event(ev.ID, ev.EventID, ev.Data, ev.Timestamp)
	}
	return []int32{}, nil
}

func (m *sniMenu) AboutToShow(id int32) (bool, *dbus.Error) {
	return false, nil
}

func (m *sniMenu) AboutToShowGroup(ids []int32) ([]int32, []int32, *dbus.Error) {
	return []int32{}, []int32{}, nil
}

func (h *linuxPlatformHooks) exportSNIObjects(conn *dbus.Conn) error {
	itemPath := dbus.ObjectPath("/StatusNotifierItem")
	menuPath := dbus.ObjectPath("/MenuBar")

	item := &sniItem{hooks: h}
	menu := &sniMenu{hooks: h}
	if err := conn.Export(item, itemPath, "org.kde.StatusNotifierItem"); err != nil {
		return err
	}
	if err := conn.Export(menu, menuPath, "com.canonical.dbusmenu"); err != nil {
		return err
	}

	itemProps := map[string]map[string]*prop.Prop{
		"org.kde.StatusNotifierItem": {
			"Category":            {Value: "ApplicationStatus", Writable: false},
			"Id":                  {Value: "dirlocker", Writable: false},
			"Title":               {Value: "目录密匣", Writable: false},
			"Status":              {Value: "Active", Writable: false},
			"WindowId":            {Value: int32(0), Writable: false},
			"IconName":            {Value: "dirlocker", Writable: false},
			"IconThemePath":       {Value: "/usr/share/icons/hicolor", Writable: false},
			"IconPixmap":          {Value: []sniPixmap{}, Writable: false},
			"OverlayIconName":     {Value: "", Writable: false},
			"OverlayIconPixmap":   {Value: []sniPixmap{}, Writable: false},
			"AttentionIconName":   {Value: "", Writable: false},
			"AttentionIconPixmap": {Value: []sniPixmap{}, Writable: false},
			"ItemIsMenu":          {Value: false, Writable: false},
			"Menu":                {Value: menuPath, Writable: false},
			"ToolTip": {
				Value: sniToolTip{
					IconName:    "dirlocker",
					IconPixmap:  []sniPixmap{},
					Title:       "目录密匣 (DirLocker)",
					Description: "跨平台轻量级目录密码保护工具",
				},
				Writable: false,
			},
		},
	}
	if _, err := prop.Export(conn, itemPath, itemProps); err != nil {
		return err
	}

	menuProps := map[string]map[string]*prop.Prop{
		"com.canonical.dbusmenu": {
			"Version":       {Value: uint32(3), Writable: false},
			"TextDirection": {Value: "ltr", Writable: false},
			"Status":        {Value: "normal", Writable: false},
		},
	}
	if _, err := prop.Export(conn, menuPath, menuProps); err != nil {
		return err
	}

	_ = conn.Export(introspect.NewIntrospectable(&introspect.Node{
		Name: string(itemPath),
		Interfaces: []introspect.Interface{
			introspect.IntrospectData,
			prop.IntrospectData,
			{Name: "org.kde.StatusNotifierItem", Methods: introspect.Methods(item)},
		},
	}), itemPath, "org.freedesktop.DBus.Introspectable")

	_ = conn.Export(introspect.NewIntrospectable(&introspect.Node{
		Name: string(menuPath),
		Interfaces: []introspect.Interface{
			introspect.IntrospectData,
			prop.IntrospectData,
			{Name: "com.canonical.dbusmenu", Methods: introspect.Methods(menu)},
		},
	}), menuPath, "org.freedesktop.DBus.Introspectable")

	return nil
}

func (h *linuxPlatformHooks) startSNITray() {
	defer func() { _ = recover() }()

	conn, err := dbus.ConnectSessionBus()
	if err != nil {
		return
	}
	busName := fmt.Sprintf("org.kde.StatusNotifierItem-%d-1", os.Getpid())
	reply, err := conn.RequestName(busName, dbus.NameFlagDoNotQueue)
	if err != nil || reply != dbus.RequestNameReplyPrimaryOwner {
		_ = conn.Close()
		return
	}

	if err := h.exportSNIObjects(conn); err != nil {
		_ = conn.Close()
		return
	}

	watcher := conn.Object("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher")
	if call := watcher.Call("org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem", 0, busName); call.Err != nil {
		_ = conn.Close()
		return
	}

	h.mu.Lock()
	h.dbusConn = conn
	h.trayActive = true
	h.mu.Unlock()
}
