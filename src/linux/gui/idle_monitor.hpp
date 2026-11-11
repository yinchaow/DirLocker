#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>

namespace dirlocker {

class IdleAndHotkeyMonitor : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit IdleAndHotkeyMonitor(QObject* parent = nullptr);
    ~IdleAndHotkeyMonitor() override;

    void start();
    void updatePolicy(int autoRelockMinutes, bool relockOnScreenLock);
    bool hotkeyRegistered() const { return hotkey_ok_; }

    bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override;

signals:
    void requestLockAll(const QString& reason);

private slots:
    void onPollTimer();
    void onScreenSaverActiveChanged(bool active);
    void onPrepareForSleep(bool goingToSleep);

private:
    void registerX11Hotkey();
    void unregisterX11Hotkey();

    QTimer poll_timer_;
    int auto_relock_minutes_ = 5;
    bool relock_on_screen_lock_ = true;
    bool idle_triggered_ = false;

    bool hotkey_ok_ = false;
    unsigned int hotkey_keycode_ = 0;
};

}  // namespace dirlocker

