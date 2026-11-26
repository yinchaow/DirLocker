//go:build linux

package backend

import (
	"fmt"
	"os"
	"testing"

	"github.com/godbus/dbus/v5"
)

func TestSNITrayDBusPropertiesAndMenuSerialization(t *testing.T) {
	if os.Getenv("DBUS_SESSION_BUS_ADDRESS") == "" {
		t.Skip("DBUS_SESSION_BUS_ADDRESS not set")
	}

	hooks := NewPlatformHooks().(*linuxPlatformHooks)
	conn, err := dbus.ConnectSessionBus()
	if err != nil {
		t.Fatalf("ConnectSessionBus failed: %v", err)
	}
	defer conn.Close()

	busName := fmt.Sprintf("org.kde.StatusNotifierItem-%d-test", os.Getpid())
	reply, err := conn.RequestName(busName, dbus.NameFlagDoNotQueue)
	if err != nil || reply != dbus.RequestNameReplyPrimaryOwner {
		t.Fatalf("RequestName failed: %v (reply=%v)", err, reply)
	}

	if err := hooks.exportSNIObjects(conn); err != nil {
		t.Fatalf("exportSNIObjects failed: %v", err)
	}

	// Simulate UKUI / KDE panel calling Properties.GetAll on StatusNotifierItem
	client, err := dbus.ConnectSessionBus()
	if err != nil {
		t.Fatalf("client ConnectSessionBus failed: %v", err)
	}
	defer client.Close()

	var allProps map[string]dbus.Variant
	obj := client.Object(busName, dbus.ObjectPath("/StatusNotifierItem"))
	if err := obj.Call("org.freedesktop.DBus.Properties.GetAll", 0, "org.kde.StatusNotifierItem").Store(&allProps); err != nil {
		t.Fatalf("Properties.GetAll(StatusNotifierItem) failed: %v", err)
	}
	tt, ok := allProps["ToolTip"]
	if !ok {
		t.Fatalf("ToolTip property missing from GetAll")
	}
	t.Logf("ToolTip variant sig=%q val=%#v", tt.Signature().String(), tt.Value())
	if vals, ok := tt.Value().([]interface{}); !ok || len(vals) != 4 {
		t.Fatalf("expected 4-field ToolTip struct, got %#v", tt.Value())
	}

	// Simulate panel querying com.canonical.dbusmenu GetLayout
	menuObj := client.Object(busName, dbus.ObjectPath("/MenuBar"))
	var menuProps map[string]dbus.Variant
	if err := menuObj.Call("org.freedesktop.DBus.Properties.GetAll", 0, "com.canonical.dbusmenu").Store(&menuProps); err != nil {
		t.Fatalf("Properties.GetAll(dbusmenu) failed: %v", err)
	}
	var rev uint32
	var layout dbusMenuLayout
	if err := menuObj.Call("com.canonical.dbusmenu.GetLayout", 0, int32(0), int32(-1), []string{}).Store(&rev, &layout); err != nil {
		t.Fatalf("dbusmenu.GetLayout failed: %v", err)
	}
	if len(layout.Children) != 4 {
		t.Fatalf("expected 4 menu items, got %d", len(layout.Children))
	}
}

func TestX11HotkeyAndIdleWorker(t *testing.T) {
	if os.Getenv("DISPLAY") == "" {
		t.Skip("DISPLAY not set")
	}
	hooks := NewPlatformHooks()
	hooks.Start(func() {}, func() {}, func() {})
	defer hooks.Stop()

	if ok, msg := hooks.RegisterHotkey("Ctrl+Alt+K"); !ok {
		t.Fatalf("RegisterHotkey(Ctrl+Alt+K) failed: %s", msg)
	}
	if ok, msg := hooks.TestHotkey("Ctrl+Alt+K"); !ok {
		t.Fatalf("TestHotkey(Ctrl+Alt+K) failed: %s", msg)
	}
	if ok, msg := hooks.TestHotkey("Ctrl+Alt+M"); !ok {
		t.Fatalf("TestHotkey(Ctrl+Alt+M) failed: %s", msg)
	}
	_, _ = hooks.IdleSeconds()
}
