#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QSpinBox>
#include <QString>

#include <functional>

class QFileSystemModel;
class QLabel;
class QPushButton;
class QTextBrowser;
class QTreeView;

namespace dirlocker {

void showMessageZh(QWidget* parent, QMessageBox::Icon icon,
                   const QString& title, const QString& text);
int askQuitRelockZh(QWidget* parent, int unlockedCount);

class MasterPasswordDialog : public QDialog {
    Q_OBJECT
public:
    explicit MasterPasswordDialog(bool requireOld, QWidget* parent = nullptr);

    QString oldPassword() const;
    QString newPassword() const;

private slots:
    void onAccept();

private:
    bool require_old_ = false;
    QLineEdit* old_edit_ = nullptr;
    QLineEdit* new_edit_ = nullptr;
    QLineEdit* confirm_edit_ = nullptr;
    QCheckBox* ack_cb_ = nullptr;
};

class LockDirDialog : public QDialog {
    Q_OBJECT
public:
    explicit LockDirDialog(const QString& dirPath, QWidget* parent = nullptr);

    bool useCustomPassword() const;
    QString customPassword() const;

private slots:
    void updateFields();
    void onAccept();

private:
    QRadioButton* global_radio_ = nullptr;
    QRadioButton* custom_radio_ = nullptr;
    QLineEdit* pw_edit_ = nullptr;
    QLineEdit* confirm_edit_ = nullptr;
    QWidget* disclaimer_box_ = nullptr;
    QCheckBox* ack_cb_ = nullptr;
};

class UnlockDirDialog : public QDialog {
    Q_OBJECT
public:
    explicit UnlockDirDialog(const QString& dirPath, bool useCustomPassword,
                             bool showOpenOption = true, QWidget* parent = nullptr);

    QString password() const;
    bool openAfterUnlock() const;

private slots:
    void onAccept();

private:
    QLineEdit* pw_edit_ = nullptr;
    QCheckBox* open_cb_ = nullptr;
};

class ChangeDirPasswordDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChangeDirPasswordDialog(const QString& dirPath, bool currentlyCustom,
                                     QWidget* parent = nullptr);

    QString currentPassword() const;
    bool useCustomPassword() const;
    QString newCustomPassword() const;

private slots:
    void onAccept();

private:
    bool target_custom_ = false;
    QLineEdit* cur_edit_ = nullptr;
    QLineEdit* new_edit_ = nullptr;
    QLineEdit* confirm_edit_ = nullptr;
    QCheckBox* ack_cb_ = nullptr;
};

class EditCustomPasswordDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditCustomPasswordDialog(const QString& dirPath, QWidget* parent = nullptr);

    QString currentPassword() const;
    QString newCustomPassword() const;

private slots:
    void onAccept();

private:
    QLineEdit* cur_edit_ = nullptr;
    QLineEdit* new_edit_ = nullptr;
    QLineEdit* confirm_edit_ = nullptr;
    QCheckBox* ack_cb_ = nullptr;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(int autoRelockMinutes, bool requireMasterOnOpen,
                   const QString& hotkey,
                   std::function<bool(const QString&, QString&)> probeHotkey,
                   QWidget* parent = nullptr);

    int autoRelockMinutes() const;
    bool requireMasterOnOpen() const;
    QString hotkey() const;
    QString masterPassword() const;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onAccept();

private:
    void applyHotkeyCandidate(const QString& raw);

    QSpinBox* minutes_spin_ = nullptr;
    QCheckBox* master_open_cb_ = nullptr;
    QLineEdit* hotkey_edit_ = nullptr;
    QLabel* hk_status_label_ = nullptr;
    QLineEdit* pw_edit_ = nullptr;
    QString chosen_hotkey_;
    bool hotkey_valid_ = true;
    std::function<bool(const QString&, QString&)> probe_hotkey_;
};

class HealthCheckDialog : public QDialog {
    Q_OBJECT
public:
    explicit HealthCheckDialog(const QString& masterPw, QWidget* parent = nullptr);

    bool changed() const { return changed_; }

private slots:
    void runScan();
    void runRepair();
    void updateRepairButtonState();

private:
    QString master_pw_;
    bool changed_ = false;
    int repairable_count_ = 0;
    int dead_record_count_ = 0;
    QLabel* summary_label_ = nullptr;
    QTextBrowser* report_view_ = nullptr;
    QCheckBox* clean_dead_cb_ = nullptr;
    QPushButton* repair_btn_ = nullptr;
};

class VerifyMasterDialog : public QDialog {
    Q_OBJECT
public:
    VerifyMasterDialog(const QString& title, const QString& prompt, QWidget* parent = nullptr);

    QString password() const;

private slots:
    void onAccept();

private:
    QLineEdit* pw_edit_ = nullptr;
};

class FolderPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit FolderPickerDialog(QWidget* parent = nullptr);

    QString selectedDirectory() const;

private:
    void navigateTo(const QString& dirPath);
    void updateSelectionPreview();

    QFileSystemModel* fs_model_ = nullptr;
    QTreeView* tree_view_ = nullptr;
    QLineEdit* path_edit_ = nullptr;
    QLabel* preview_label_ = nullptr;
    QPushButton* ok_btn_ = nullptr;
    QString current_dir_;
    QString chosen_dir_;
};

}  // namespace dirlocker

