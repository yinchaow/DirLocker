#include "idle_monitor.hpp"

#include "../../core/security_util.hpp"

#include <QCoreApplication>
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

bool resolve_x11_hotkey(Display* dpy, const QString& raw, unsigned int& out_mods,
                        unsigned int& out_kc, QString& normalized, QString* err_out) {
    HotkeySpec spec;
    std::string perr;
    if (!HotkeySpec::parse(raw.toStdString(), spec, &perr)) {
        if (err_out) *err_out = QString::fromStdString(perr);
        return false;
    }
    normalized = QString::fromStdString(spec.to_string());

    KeySym sym = NoSymbol;
    if (spec.key.size() == 1) {
        char c = spec.key[0];
        if (c >= 'A' && c <= 'Z') {
            sym = static_cast<KeySym>(XK_A + (c - 'A'));
        } else if (c >= '0' && c <= '9') {
            sym = static_cast<KeySym>(XK_0 + (c - '0'));
        }
    } else if (spec.key.size() >= 2 && spec.key[0] == 'F') {
        int fn = std::stoi(spec.key.substr(1));
        if (fn >= 1 && fn <= 12) {
            sym = static_cast<KeySym>(XK_F1 + (fn - 1));
        }
    }
    if (sym == NoSymbol) {
        if (err_out) *err_out = QStringLiteral("无法识别的主键");
        return false;
    }

    KeyCode kc = dpy ? XKeysymToKeycode(dpy, sym) : 0;
    if (dpy && kc == 0) {
        if (err_out) *err_out = QStringLiteral("当前键盘映射不支持该按键");
        return false;
    }

    out_kc = static_cast<unsigned int>(kc);
    out_mods = (spec.ctrl ? ControlMask : 0u) |
               (spec.alt ? Mod1Mask : 0u) |
               (spec.shift ? ShiftMask : 0u);
    return true;
}

void ungrab_combos(Display* dpy, unsigned int kc, unsigned int base_mods) {
    if (!dpy || kc == 0) return;
    Window root = DefaultRootWindow(dpy);
    const unsigned int lock_combos[4] = {
        base_mods,
        base_mods | Mod2Mask,
        base_mods | LockMask,
        base_mods | Mod2Mask | LockMask
    };
    for (unsigned int mods : lock_combos) {
        XUngrabKey(dpy, static_cast<int>(kc), mods, root);
    }
    XFlush(dpy);
}

bool try_grab_combos(Display* dpy, unsigned int kc, unsigned int base_mods) {
    if (!dpy || kc == 0) return false;
    Window root = DefaultRootWindow(dpy);
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
    if (g_x11_grab_error) {
        ungrab_combos(dpy, kc, base_mods);
        return false;
    }
    return true;
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
    poll_timer_.start(3000);
}

void IdleAndHotkeyMonitor::updatePolicy(int autoRelockMinutes) {
    auto_relock_minutes_ = autoRelockMinutes;
    idle_triggered_ = false;
}

bool IdleAndHotkeyMonitor::setHotkey(const QString& hotkey) {
    QString norm = QString::fromStdString(HotkeySpec::normalize(hotkey.toStdString(), "Ctrl+Alt+K"));
    hotkey_str_ = norm;
    unregisterX11Hotkey();
    if (!hotkey_paused_) {
        registerX11Hotkey();
    }
    return hotkey_ok_;
}

void IdleAndHotkeyMonitor::setHotkeyPaused(bool paused) {
    if (hotkey_paused_ == paused) return;
    hotkey_paused_ = paused;
    if (paused) {
        unregisterX11Hotkey();
    } else {
        registerX11Hotkey();
    }
}

bool IdleAndHotkeyMonitor::testHotkeyAvailable(const QString& hotkey, QString* statusMsg) const {
    if (!QX11Info::isPlatformX11()) {
        HotkeySpec spec;
        std::string err;
        if (!HotkeySpec::parse(hotkey.toStdString(), spec, &err)) {
            if (statusMsg) *statusMsg = QString::fromStdString(err);
            return false;
        }
        if (statusMsg) *statusMsg = QStringLiteral("非 X11 会话，保存后将记录此设置");
        return true;
    }
    Display* dpy = QX11Info::display();
    unsigned int mods = 0, kc = 0;
    QString norm, perr;
    if (!resolve_x11_hotkey(dpy, hotkey, mods, kc, norm, &perr)) {
        if (statusMsg) *statusMsg = perr;
        return false;
    }
    if (hotkey_ok_ && kc == hotkey_keycode_ && mods == hotkey_mods_) {
        if (statusMsg) *statusMsg = QStringLiteral("%1 可用（当前已注册生效）").arg(norm);
        return true;
    }
    bool ok = try_grab_combos(dpy, kc, mods);
    if (ok) {
        ungrab_combos(dpy, kc, mods);
        if (statusMsg) *statusMsg = QStringLiteral("%1 可用（点保存后生效）").arg(norm);
        return true;
    }
    if (statusMsg) *statusMsg = QStringLiteral("%1 已被系统或其他程序占用，请更换组合").arg(norm);
    return false;
}

void IdleAndHotkeyMonitor::registerX11Hotkey() {
    if (!QX11Info::isPlatformX11()) return;
    Display* dpy = QX11Info::display();
    if (!dpy) return;

    unsigned int mods = 0, kc = 0;
    QString norm;
    if (!resolve_x11_hotkey(dpy, hotkey_str_, mods, kc, norm, nullptr)) {
        hotkey_ok_ = false;
        return;
    }
    hotkey_str_ = norm;
    hotkey_keycode_ = kc;
    hotkey_mods_ = mods;
    hotkey_ok_ = try_grab_combos(dpy, kc, mods);
}

void IdleAndHotkeyMonitor::unregisterX11Hotkey() {
    if (!hotkey_ok_ || hotkey_keycode_ == 0 || !QX11Info::isPlatformX11()) return;
    Display* dpy = QX11Info::display();
    ungrab_combos(dpy, hotkey_keycode_, hotkey_mods_);
    hotkey_ok_ = false;
}

bool IdleAndHotkeyMonitor::nativeEventFilter(const QByteArray& eventType, void* message, long*) {
    if (!hotkey_ok_ || hotkey_paused_ || eventType != "xcb_generic_event_t" || !message) {
        return false;
    }
    auto* ev = static_cast<xcb_generic_event_t*>(message);
    if ((ev->response_type & ~0x80) == XCB_KEY_PRESS) {
        auto* kp = reinterpret_cast<xcb_key_press_event_t*>(ev);
        const uint16_t mask = static_cast<uint16_t>(kp->state & (ControlMask | Mod1Mask | ShiftMask));
        if (kp->detail == hotkey_keycode_ && mask == hotkey_mods_) {
            emit requestLockAll(QStringLiteral("快捷键 %1 一键锁定").arg(hotkey_str_));
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

}  // namespace dirlocker
