#include "idle_monitor.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QX11Info>

#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <X11/keysym.h>

#ifdef KeyPress
#undef KeyPress
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif

namespace dirlocker {

namespace {

bool g_x11_grab_error = false;
int (*g_prev_x11_err_handler)(Display*, XErrorEvent*) = nullptr;

int grab_x11_error_handler(Display*, XErrorEvent*) {
    g_x11_grab_error = true;
    return 0;
}

}  // namespace

IdleAndHotkeyMonitor::IdleAndHotkeyMonitor(QObject* parent) : QObject(parent) {
    connect(&poll_timer_, &QTimer::timeout, this, &IdleAndHotkeyMonitor::onPollTimer);
}

IdleAndHotkeyMonitor::~IdleAndHotkeyMonitor() {
    unregisterX11Hotkey();
    if (auto* app = QCoreApplication::instance()) {
        app->removeNativeEventFilter(this);
    }
}

void IdleAndHotkeyMonitor::start() {
    if (auto* app = QCoreApplication::instance()) {
        app->installNativeEventFilter(this);
    }
    registerX11Hotkey();

    // Listen to UKUI / freedesktop / GNOME screensaver lock signals.
    auto session = QDBusConnection::sessionBus();
    session.connect("org.ukui.ScreenSaver", "/org/ukui/ScreenSaver",
                    "org.ukui.ScreenSaver", "ActiveChanged",
                    this, SLOT(onScreenSaverActiveChanged(bool)));
    session.connect("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                    "org.freedesktop.ScreenSaver", "ActiveChanged",
                    this, SLOT(onScreenSaverActiveChanged(bool)));
    session.connect("org.gnome.ScreenSaver", "/org/gnome/ScreenSaver",
                    "org.gnome.ScreenSaver", "ActiveChanged",
                    this, SLOT(onScreenSaverActiveChanged(bool)));

    QDBusConnection::systemBus().connect(
        "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "PrepareForSleep",
        this, SLOT(onPrepareForSleep(bool)));

    poll_timer_.start(3000);
}

void IdleAndHotkeyMonitor::updatePolicy(int autoRelockMinutes, bool relockOnScreenLock) {
    auto_relock_minutes_ = autoRelockMinutes;
    relock_on_screen_lock_ = relockOnScreenLock;
    idle_triggered_ = false;
}

void IdleAndHotkeyMonitor::registerX11Hotkey() {
    if (!QX11Info::isPlatformX11()) return;
    Display* dpy = QX11Info::display();
    if (!dpy) return;

    KeyCode kc = XKeysymToKeycode(dpy, XK_L);
    if (kc == 0) return;

    hotkey_keycode_ = kc;
    Window root = DefaultRootWindow(dpy);
    const unsigned int base_mods = ControlMask | Mod1Mask;
    const unsigned int lock_combos[4] = {
        base_mods,
        base_mods | Mod2Mask,
        base_mods | LockMask,
        base_mods | Mod2Mask | LockMask
    };

    g_x11_grab_error = false;
    g_prev_x11_err_handler = XSetErrorHandler(grab_x11_error_handler);
    for (unsigned int mods : lock_combos) {
        XGrabKey(dpy, static_cast<int>(kc), mods, root, True, GrabModeAsync, GrabModeAsync);
    }
    XSync(dpy, False);
    XSetErrorHandler(g_prev_x11_err_handler);

    hotkey_ok_ = !g_x11_grab_error;
}

void IdleAndHotkeyMonitor::unregisterX11Hotkey() {
    if (!hotkey_ok_ || hotkey_keycode_ == 0 || !QX11Info::isPlatformX11()) return;
    Display* dpy = QX11Info::display();
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    const unsigned int base_mods = ControlMask | Mod1Mask;
    const unsigned int lock_combos[4] = {
        base_mods,
        base_mods | Mod2Mask,
        base_mods | LockMask,
        base_mods | Mod2Mask | LockMask
    };
    for (unsigned int mods : lock_combos) {
        XUngrabKey(dpy, static_cast<int>(hotkey_keycode_), mods, root);
    }
    XFlush(dpy);
    hotkey_ok_ = false;
}

bool IdleAndHotkeyMonitor::nativeEventFilter(const QByteArray& eventType, void* message, long*) {
    if (!hotkey_ok_ || eventType != "xcb_generic_event_t" || !message) return false;
    auto* ev = static_cast<xcb_generic_event_t*>(message);
    if ((ev->response_type & ~0x80) == XCB_KEY_PRESS) {
        auto* kp = reinterpret_cast<xcb_key_press_event_t*>(ev);
        const uint16_t mask = static_cast<uint16_t>(kp->state & ~(Mod2Mask | LockMask));
        if (kp->detail == hotkey_keycode_ && mask == (ControlMask | Mod1Mask)) {
            emit requestLockAll(QStringLiteral("快捷键 Ctrl+Alt+L 一键锁定"));
            return false;
        }
    }
    return false;
}

void IdleAndHotkeyMonitor::onPollTimer() {
    if (auto_relock_minutes_ <= 0) {
        idle_triggered_ = false;
        return;
    }
    if (!QX11Info::isPlatformX11()) return;

    Display* dpy = QX11Info::display();
    if (!dpy) return;

    int event_base = 0;
    int error_base = 0;
    if (!XScreenSaverQueryExtension(dpy, &event_base, &error_base)) return;

    XScreenSaverInfo* info = XScreenSaverAllocInfo();
    if (!info) return;

    if (XScreenSaverQueryInfo(dpy, DefaultRootWindow(dpy), info)) {
        const unsigned long threshold_ms = static_cast<unsigned long>(auto_relock_minutes_) * 60000UL;
        if (info->idle >= threshold_ms) {
            if (!idle_triggered_) {
                idle_triggered_ = true;
                emit requestLockAll(QStringLiteral("系统闲置超过 %1 分钟自动回锁").arg(auto_relock_minutes_));
            }
        } else {
            idle_triggered_ = false;
        }
    }
    XFree(info);
}

void IdleAndHotkeyMonitor::onScreenSaverActiveChanged(bool active) {
    if (active && relock_on_screen_lock_) {
        emit requestLockAll(QStringLiteral("系统锁屏自动回锁"));
    }
}

void IdleAndHotkeyMonitor::onPrepareForSleep(bool goingToSleep) {
    if (goingToSleep && relock_on_screen_lock_) {
        emit requestLockAll(QStringLiteral("系统休眠前自动回锁"));
    }
}

}  // namespace dirlocker

