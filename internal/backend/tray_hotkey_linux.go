//go:build linux

package backend

/*
#cgo LDFLAGS: -lX11 -ldl
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static int dl_grab_error = 0;
static int dl_x_err_handler(Display* dpy, XErrorEvent* ev) {
    (void)dpy;
    if (ev && (ev->error_code == BadAccess || ev->error_code == BadValue)) {
        dl_grab_error = 1;
    }
    return 0;
}

static int dl_grab_combo(Display* dpy, KeyCode code, unsigned int base_mods) {
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

static int dl_xss_idle_ms(unsigned long* out_ms) {
    static void* handle = NULL;
    static fn_xss_alloc p_alloc = NULL;
    static fn_xss_query p_query = NULL;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        handle = dlopen("libXss.so.1", RTLD_LAZY);
        if (handle) {
            p_alloc = (fn_xss_alloc)dlsym(handle, "XScreenSaverAllocInfo");
            p_query = (fn_xss_query)dlsym(handle, "XScreenSaverQueryInfo");
        }
    }
    if (!p_alloc || !p_query) return 0;
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return 0;
    DL_XScreenSaverInfo* info = p_alloc();
    if (!info) {
        XCloseDisplay(dpy);
        return 0;
    }
    int ok = 0;
    if (p_query(dpy, DefaultRootWindow(dpy), info) != 0) {
        *out_ms = info->idle;
        ok = 1;
    }
    XFree(info);
    XCloseDisplay(dpy);
    return ok;
}
*/
import "C"

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"
	"unsafe"

	"dirlocker/internal/core"
	"github.com/godbus/dbus/v5"
	"github.com/godbus/dbus/v5/introspect"
	"github.com/godbus/dbus/v5/prop"
)

type linuxPlatformHooks struct {
	mu           sync.Mutex
	onShow       func()
	onLockAll    func()
	onQuit       func()
	stopCh       chan struct{}
	stopOnce     sync.Once
	dpy          *C.Display
	grabbedCode  C.KeyCode
	grabbedMods  C.uint
	activeSpec   core.HotkeySpec
	hotkeyActive bool
	dbusConn     *dbus.Conn
	trayActive   bool
}

func NewPlatformHooks() PlatformHooks {
	return &linuxPlatformHooks{
		stopCh: make(chan struct{}),
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
	if os.Getenv("DISPLAY") != "" {
		h.dpy = C.XOpenDisplay(nil)
	}
	h.mu.Unlock()

	if h.dpy != nil {
		go h.runX11Loop()
	}
	go h.startSNITray()
}

func (h *linuxPlatformHooks) runX11Loop() {
	ticker := time.NewTicker(40 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-h.stopCh:
			return
		case <-ticker.C:
			h.mu.Lock()
			dpy := h.dpy
			active := h.hotkeyActive
			wantCode := h.grabbedCode
			wantMods := h.grabbedMods
			onLockAll := h.onLockAll
			if dpy == nil {
				h.mu.Unlock()
				return
			}
			var gotCode C.KeyCode
			var gotState C.uint
			fired := false
			for C.dl_poll_keypress(dpy, &gotCode, &gotState) != 0 {
				if active && gotCode == wantCode && gotState == wantMods {
					fired = true
				}
			}
			h.mu.Unlock()
			if fired && onLockAll != nil {
				go onLockAll()
			}
		}
	}
}

func (h *linuxPlatformHooks) Stop() {
	h.stopOnce.Do(func() {
		close(h.stopCh)
		h.mu.Lock()
		if h.dpy != nil {
			if h.hotkeyActive {
				C.dl_ungrab_combo(h.dpy, h.grabbedCode, h.grabbedMods)
				h.hotkeyActive = false
			}
			C.XCloseDisplay(h.dpy)
			h.dpy = nil
		}
		if h.dbusConn != nil {
			_ = h.dbusConn.Close()
			h.dbusConn = nil
		}
		h.trayActive = false
		h.mu.Unlock()
	})
}

func (h *linuxPlatformHooks) RegisterHotkey(hotkey string) (bool, string) {
	spec, err := core.ParseHotkey(hotkey)
	if err != nil {
		return false, err.Error()
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.dpy == nil {
		return false, "当前桌面会话未提供 X11 显示连接，全局快捷键暂不可用"
	}
	if h.hotkeyActive {
		C.dl_ungrab_combo(h.dpy, h.grabbedCode, h.grabbedMods)
		h.hotkeyActive = false
	}
	code, mods, ok := hotkeySpecToX11(h.dpy, spec)
	if !ok {
		return false, "无法映射快捷键按键: " + spec.String()
	}
	if C.dl_grab_combo(h.dpy, code, mods) == 0 {
		return false, fmt.Sprintf("全局快捷键 %s 注册失败，可能已被桌面环境或其他程序占用", spec.String())
	}
	h.grabbedCode = code
	h.grabbedMods = mods
	h.activeSpec = spec
	h.hotkeyActive = true
	return true, ""
}

func (h *linuxPlatformHooks) TestHotkey(hotkey string) (bool, string) {
	spec, err := core.ParseHotkey(hotkey)
	if err != nil {
		return false, err.Error()
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.dpy == nil {
		return false, "当前会话未检测到 X11 显示连接"
	}
	if h.hotkeyActive && h.activeSpec.String() == spec.String() {
		return true, "当前快捷键已生效，无冲突"
	}
	code, mods, ok := hotkeySpecToX11(h.dpy, spec)
	if !ok {
		return false, "不支持该按键组合"
	}
	if C.dl_grab_combo(h.dpy, code, mods) == 0 {
		return false, fmt.Sprintf("快捷键 %s 已被系统或其他程序占用，请更换组合", spec.String())
	}
	C.dl_ungrab_combo(h.dpy, code, mods)
	return true, fmt.Sprintf("快捷键 %s 可用，未发现系统冲突", spec.String())
}

func (h *linuxPlatformHooks) IdleSeconds() (int, bool) {
	// 1. Try XScreenSaver extension (works natively on Kylin UKUI / Xorg)
	var ms C.ulong
	if C.dl_xss_idle_ms(&ms) != 0 {
		return int(ms / 1000), true
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
	return nil, nil
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

func (h *linuxPlatformHooks) startSNITray() {
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

	itemPath := dbus.ObjectPath("/StatusNotifierItem")
	menuPath := dbus.ObjectPath("/MenuBar")

	item := &sniItem{hooks: h}
	menu := &sniMenu{hooks: h}
	_ = conn.Export(item, itemPath, "org.kde.StatusNotifierItem")
	_ = conn.Export(menu, menuPath, "com.canonical.dbusmenu")

	itemProps := map[string]map[string]*prop.Prop{
		"org.kde.StatusNotifierItem": {
			"Category":   {Value: "ApplicationStatus", Writable: false},
			"Id":         {Value: "dirlocker", Writable: false},
			"Title":      {Value: "目录密匣", Writable: false},
			"Status":     {Value: "Active", Writable: false},
			"IconName":   {Value: "dirlocker", Writable: false},
			"ItemIsMenu": {Value: false, Writable: false},
			"Menu":       {Value: menuPath, Writable: false},
			"ToolTip": {
				Value: struct {
					IconName    string
					IconPixmap  []struct{}
					Title       string
					Description string
				}{
					IconName:    "dirlocker",
					Title:       "目录密匣 (DirLocker)",
					Description: "跨平台轻量级目录密码保护工具",
				},
				Writable: false,
			},
		},
	}
	_, _ = prop.Export(conn, itemPath, itemProps)

	menuProps := map[string]map[string]*prop.Prop{
		"com.canonical.dbusmenu": {
			"Version":       {Value: uint32(3), Writable: false},
			"TextDirection": {Value: "ltr", Writable: false},
			"Status":        {Value: "normal", Writable: false},
		},
	}
	_, _ = prop.Export(conn, menuPath, menuProps)

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
