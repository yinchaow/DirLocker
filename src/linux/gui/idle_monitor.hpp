#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QTimer>

namespace dirlocker {

class IdleAndHotkeyMonitor : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit IdleAndHotkeyMonitor(QObject* parent = nullptr);
    ~IdleAndHotkeyMonitor() override;

    void start();
    void updatePolicy(int autoRelockMinutes);
    bool setHotkey(const QString& hotkey);
    void setHotkeyPaused(bool paused);
    bool testHotkeyAvailable(const QString& hotkey, QString* statusMsg = nullptr) const;
    bool hotkeyRegistered() const { return hotkey_ok_; }
    QString currentHotkey() const { return hotkey_str_; }

    bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override;

signals:
    void requestLockAll(const QString& reason);

private slots:
    void onPollTimer();

private:
    void registerX11Hotkey();
    void unregisterX11Hotkey();

    QTimer poll_timer_;
    int auto_relock_minutes_ = 5;
    bool idle_triggered_ = false;

    QString hotkey_str_ = QStringLiteral("Ctrl+Alt+L");
    bool hotkey_paused_ = false;
    bool hotkey_ok_ = false;
    unsigned int hotkey_keycode_ = 0;
    unsigned int hotkey_mods_ = 0;
};

}  // namespace dirlocker
