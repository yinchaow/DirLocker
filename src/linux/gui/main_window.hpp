#pragma once

#include "idle_monitor.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTableWidget>

#include <cstddef>
#include <vector>

namespace dirlocker {

class DirTableDelegate;
class EmptyTableOverlay;

struct UiDirRow {
    QString path;
    bool locked = false;
    bool use_custom_password = false;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void handleExternalCommand(const QString& action, const QString& path);
    bool ensureMasterPasswordInitialized();
    void refreshState();
    void showMainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onAddAndLock();
    void onUnlockSelected();
    void onLockSelected();
    void onLockAll();
    void onChangeDirPassword();
    void onEditCustomPassword();
    void onRemoveSelected();
    void onChangeMasterPassword();
    void onOpenSettings();
    void onHealthCheck();
    void onShowRecoveryGuide();
    void onShowAbout();
    void onTableClicked(const QModelIndex& index);
    void onTableDoubleClicked(int row, int col);
    void onTableContextMenu(const QPoint& pos);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onAutoLockRequested(const QString& reason);
    void onQuitRequested();

private:
    void setupUi();
    void setupTray();
    void hideAndLockMainWindow();
    void updateHotkeyDisplay();
    void openFolderInFileManager(const QString& path);

    bool lockNewPathFlow(const QString& path);
    bool lockPathsBatchFlow(const QStringList& paths);
    bool unlockPathFlow(const QString& path, bool useCustomPassword);
    void lockAllInternal(const QString& reason, bool showNotifyWhenZero);

    QIcon createAppIcon(bool lockedTint = true) const;
    int selectedRowIndex() const;
    void rebuildTableView();
    void showRowMenu(int visRow, const QPoint& anchorGlobalPos, bool alignRight, bool moreMenuOnly);

    QLineEdit* search_edit_ = nullptr;
    QTableWidget* table_ = nullptr;
    DirTableDelegate* delegate_ = nullptr;
    EmptyTableOverlay* empty_overlay_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QLabel* policy_label_ = nullptr;
    QPushButton* lock_all_btn_ = nullptr;
    QAction* tray_lock_all_act_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    IdleAndHotkeyMonitor* monitor_ = nullptr;

    bool has_master_password_ = false;
    int auto_relock_minutes_ = 5;
    bool require_master_on_open_ = false;
    bool main_window_unlocked_ = false;
    bool main_auth_prompt_open_ = false;
    QString hotkey_ = QStringLiteral("Ctrl+Alt+L");
    std::vector<UiDirRow> rows_;
    std::vector<size_t> visible_indices_;
    bool really_quit_ = false;
};

}  // namespace dirlocker
