#include "password_dialog.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

namespace dirlocker {

namespace {

QIcon make_eye_icon(bool openEye) {
    QIcon icon;
    for (int px : {16, 24, 32}) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = static_cast<qreal>(px) / 16.0;
        QPen pen(QColor(95, 99, 104), 1.35 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        QPainterPath eye;
        eye.moveTo(2.0 * s, 8.0 * s);
        eye.quadTo(8.0 * s, 2.8 * s, 14.0 * s, 8.0 * s);
        eye.quadTo(8.0 * s, 13.2 * s, 2.0 * s, 8.0 * s);
        p.drawPath(eye);

        p.drawEllipse(QPointF(8.0 * s, 8.0 * s), 2.1 * s, 2.1 * s);
        if (!openEye) {
            p.drawLine(QPointF(3.0 * s, 13.0 * s), QPointF(13.0 * s, 3.0 * s));
        }
        icon.addPixmap(pm);
    }
    return icon;
}

QLineEdit* make_pw_edit(QWidget* parent, const QString& placeholder = QString()) {
    auto* e = new QLineEdit(parent);
    e->setEchoMode(QLineEdit::Password);
    e->setPlaceholderText(placeholder);

    const QIcon showIcon = make_eye_icon(true);
    const QIcon hideIcon = make_eye_icon(false);
    auto* toggleAct = e->addAction(showIcon, QLineEdit::TrailingPosition);
    toggleAct->setToolTip(QStringLiteral("显示密码"));
    QObject::connect(toggleAct, &QAction::triggered, e, [e, toggleAct, showIcon, hideIcon]() {
        const bool showing = (e->echoMode() == QLineEdit::Normal);
        e->setEchoMode(showing ? QLineEdit::Password : QLineEdit::Normal);
        toggleAct->setIcon(showing ? showIcon : hideIcon);
        toggleAct->setToolTip(showing ? QStringLiteral("显示密码") : QStringLiteral("隐藏密码"));
    });
    return e;
}

QDialogButtonBox* make_button_box(QWidget* parent,
                                  const QString& okText = QStringLiteral("确定"),
                                  const QString& cancelText = QStringLiteral("取消")) {
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, parent);
    if (auto* okBtn = box->button(QDialogButtonBox::Ok)) {
        okBtn->setText(okText);
    }
    if (auto* cancelBtn = box->button(QDialogButtonBox::Cancel)) {
        cancelBtn->setText(cancelText);
    }
    return box;
}

QFrame* make_password_disclaimer(QWidget* parent, QCheckBox*& outCb) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("pwDisclaimerBox"));
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    frame->setStyleSheet(QStringLiteral(
        "#pwDisclaimerBox { background: #fffbeb; border: 1px solid #f6c453; border-radius: 6px; }"
        "#pwDisclaimerBox:disabled { background: #f8fafc; border-color: #e2e8f0; }"));
    auto* boxLayout = new QVBoxLayout(frame);
    boxLayout->setContentsMargins(10, 8, 10, 8);
    boxLayout->setSpacing(6);

    auto* warnLabel = new QLabel(
        QStringLiteral(
            "<span style=\"color:#b45309;\"><b>重要提醒：</b>本软件不提供找回密码功能，请务必妥善牢记密码；"
            "若遗忘密码将无法通过程序直接解锁，由此造成的访问不便与相关风险由使用者自行承担。</span>"),
        frame);
    warnLabel->setTextFormat(Qt::RichText);
    warnLabel->setWordWrap(true);
    warnLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    warnLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    boxLayout->addWidget(warnLabel);

    outCb = new QCheckBox(
        QStringLiteral("我已牢记上述密码，并知晓忘记密码无法找回的相关风险"), frame);
    outCb->setStyleSheet(QStringLiteral("QCheckBox { color: #1f2328; font-weight: 500; }"));
    boxLayout->addWidget(outCb);
    return frame;
}

bool ensure_disclaimer_checked(QWidget* parent, QCheckBox* cb) {
    if (cb && cb->isChecked()) return true;
    showMessageZh(parent, QMessageBox::Warning, QStringLiteral("请先确认"),
                  QStringLiteral("请先阅读重要提醒并勾选确认“已牢记密码并知晓忘记密码的风险”。"));
    if (cb) cb->setFocus();
    return false;
}

}  // namespace

void showMessageZh(QWidget* parent, QMessageBox::Icon icon,
                   const QString& title, const QString& text) {
    QMessageBox box(icon, title, text, QMessageBox::NoButton, parent);
    auto* okBtn = box.addButton(QStringLiteral("确定"), QMessageBox::AcceptRole);
    box.setDefaultButton(okBtn);
    box.exec();
}

int askQuitRelockZh(QWidget* parent, int unlockedCount) {
    QMessageBox box(
        QMessageBox::Question,
        QStringLiteral("退出确认"),
        QStringLiteral("当前仍有 %1 个受管目录处于解锁状态，是否全部重新锁定后再退出？").arg(unlockedCount),
        QMessageBox::NoButton,
        parent);
    auto* lockQuitBtn = box.addButton(QStringLiteral("锁定并退出"), QMessageBox::YesRole);
    auto* directQuitBtn = box.addButton(QStringLiteral("直接退出"), QMessageBox::NoRole);
    auto* cancelBtn = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(lockQuitBtn);
    box.exec();
    if (box.clickedButton() == lockQuitBtn) return 1;
    if (box.clickedButton() == directQuitBtn) return 0;
    Q_UNUSED(cancelBtn);
    return -1;
}

MasterPasswordDialog::MasterPasswordDialog(bool requireOld, QWidget* parent)
    : QDialog(parent), require_old_(requireOld) {
    setWindowTitle(requireOld ? QStringLiteral("修改主密码") : QStringLiteral("设置主密码"));
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* hint = new QLabel(
        requireOld
            ? QStringLiteral("<span style=\"color:#1f2328;\">修改主密码后，所有采用</span><b style=\"color:#0d47a1;\">主密码</b><span style=\"color:#1f2328;\">方式的目录将自动使用新密码。</span>")
            : QStringLiteral("<span style=\"color:#1f2328;\">首次使用目录密匣，请先设置</span><b style=\"color:#0d47a1;\">主密码</b><span style=\"color:#1f2328;\">（至少 4 位），作为默认解锁密码。</span>"),
        this);
    hint->setTextFormat(Qt::RichText);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout();
    if (require_old_) {
        old_edit_ = make_pw_edit(this, QStringLiteral("请输入原主密码"));
        form->addRow(QStringLiteral("原主密码:"), old_edit_);
    }
    new_edit_ = make_pw_edit(this, QStringLiteral("至少 4 个字符"));
    confirm_edit_ = make_pw_edit(this, QStringLiteral("再次输入新主密码"));
    form->addRow(QStringLiteral("新主密码:"), new_edit_);
    form->addRow(QStringLiteral("确认密码:"), confirm_edit_);
    layout->addLayout(form);

    layout->addWidget(make_password_disclaimer(this, ack_cb_));

    auto* buttons = make_button_box(this, QStringLiteral("确定"), QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &MasterPasswordDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString MasterPasswordDialog::oldPassword() const {
    return old_edit_ ? old_edit_->text() : QString();
}

QString MasterPasswordDialog::newPassword() const {
    return new_edit_->text();
}

void MasterPasswordDialog::onAccept() {
    if (require_old_ && old_edit_->text().isEmpty()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("请输入原主密码。"));
        old_edit_->setFocus();
        return;
    }
    if (new_edit_->text().size() < 4) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("新密码长度至少需要 4 个字符。"));
        new_edit_->setFocus();
        return;
    }
    if (new_edit_->text() != confirm_edit_->text()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("两次输入的新密码不一致。"));
        confirm_edit_->selectAll();
        confirm_edit_->setFocus();
        return;
    }
    if (!ensure_disclaimer_checked(this, ack_cb_)) return;
    accept();
}

LockDirDialog::LockDirDialog(const QString& dirPath, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("锁定目录"));
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* pathLabel = new QLabel(
        QStringLiteral("<span style=\"color:#5f6368;\">目标目录：</span><span style=\"color:#1f2328;\">") +
            dirPath.toHtmlEscaped() + QStringLiteral("</span>"),
        this);
    pathLabel->setTextFormat(Qt::RichText);
    pathLabel->setWordWrap(true);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(pathLabel);

    global_radio_ = new QRadioButton(QStringLiteral("使用主密码（默认，直接锁定）"), this);
    custom_radio_ = new QRadioButton(QStringLiteral("设置独立密码"), this);
    global_radio_->setChecked(true);
    layout->addWidget(global_radio_);
    layout->addWidget(custom_radio_);

    auto* form = new QFormLayout();
    pw_edit_ = make_pw_edit(this, QStringLiteral("至少 4 个字符"));
    confirm_edit_ = make_pw_edit(this, QStringLiteral("再次输入独立密码"));
    form->addRow(QStringLiteral("独立密码:"), pw_edit_);
    form->addRow(QStringLiteral("确认密码:"), confirm_edit_);
    layout->addLayout(form);

    disclaimer_box_ = make_password_disclaimer(this, ack_cb_);
    layout->addWidget(disclaimer_box_);

    connect(global_radio_, &QRadioButton::toggled, this, &LockDirDialog::updateFields);
    connect(custom_radio_, &QRadioButton::toggled, this, &LockDirDialog::updateFields);

    auto* buttons = make_button_box(this, QStringLiteral("确定锁定"), QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &LockDirDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    updateFields();
}

bool LockDirDialog::useCustomPassword() const {
    return custom_radio_->isChecked();
}

QString LockDirDialog::customPassword() const {
    return pw_edit_->text();
}

void LockDirDialog::updateFields() {
    bool custom = custom_radio_->isChecked();
    pw_edit_->setEnabled(custom);
    confirm_edit_->setEnabled(custom);
    if (disclaimer_box_) {
        disclaimer_box_->setVisible(custom);
        disclaimer_box_->setEnabled(custom);
    }
    if (ack_cb_) {
        ack_cb_->setEnabled(custom);
        if (!custom) ack_cb_->setChecked(false);
    }
    if (!custom) {
        pw_edit_->clear();
        confirm_edit_->clear();
    } else if (isVisible()) {
        pw_edit_->setFocus();
    }
    if (layout()) layout()->activate();
    if (isVisible()) {
        adjustSize();
    }
}

void LockDirDialog::onAccept() {
    if (custom_radio_->isChecked()) {
        if (pw_edit_->text().size() < 4) {
            showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("独立密码长度至少需要 4 个字符。"));
            pw_edit_->setFocus();
            return;
        }
        if (pw_edit_->text() != confirm_edit_->text()) {
            showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("两次输入的独立密码不一致。"));
            confirm_edit_->selectAll();
            confirm_edit_->setFocus();
            return;
        }
        if (!ensure_disclaimer_checked(this, ack_cb_)) return;
    }
    accept();
}

UnlockDirDialog::UnlockDirDialog(const QString& dirPath, bool useCustomPassword,
                                 bool showOpenOption, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("输入目录密码"));
    setMinimumWidth(440);
    if (!parent) {
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
    }

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* pathLabel = new QLabel(
        QStringLiteral("<span style=\"color:#5f6368;\">目标目录：</span><span style=\"color:#1f2328;\">") +
            dirPath.toHtmlEscaped() + QStringLiteral("</span>"),
        this);
    pathLabel->setTextFormat(Qt::RichText);
    pathLabel->setWordWrap(true);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(pathLabel);

    auto* modeLabel = new QLabel(
        useCustomPassword
            ? QStringLiteral("<span style=\"color:#5f6368;\">密码类型：</span><b style=\"color:#0d47a1;\">独立密码</b>")
            : QStringLiteral("<span style=\"color:#5f6368;\">密码类型：</span><b style=\"color:#0d47a1;\">主密码</b>"),
        this);
    modeLabel->setTextFormat(Qt::RichText);
    layout->addWidget(modeLabel);

    auto* form = new QFormLayout();
    pw_edit_ = make_pw_edit(this, QStringLiteral("请输入密码"));
    form->addRow(QStringLiteral("密码:"), pw_edit_);
    layout->addLayout(form);

    if (showOpenOption) {
        open_cb_ = new QCheckBox(QStringLiteral("解锁后自动在文件管理器中打开该目录"), this);
        open_cb_->setChecked(true);
        layout->addWidget(open_cb_);
    }

    auto* buttons = make_button_box(this, QStringLiteral("解锁"), QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &UnlockDirDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    pw_edit_->setFocus();
    if (!parent) {
        QTimer::singleShot(0, this, [this]() {
            raise();
            activateWindow();
            pw_edit_->setFocus();
        });
    }
}

QString UnlockDirDialog::password() const {
    return pw_edit_->text();
}

bool UnlockDirDialog::openAfterUnlock() const {
    return open_cb_ ? open_cb_->isChecked() : false;
}

void UnlockDirDialog::onAccept() {
    if (pw_edit_->text().isEmpty()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("请输入密码。"));
        pw_edit_->setFocus();
        return;
    }
    accept();
}

ChangeDirPasswordDialog::ChangeDirPasswordDialog(const QString& dirPath, bool currentlyCustom, QWidget* parent)
    : QDialog(parent), target_custom_(!currentlyCustom) {
    setWindowTitle(QStringLiteral("切换密码方式"));
    setMinimumWidth(470);

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* pathLabel = new QLabel(
        QStringLiteral("<span style=\"color:#5f6368;\">目标目录：</span><span style=\"color:#1f2328;\">") +
            dirPath.toHtmlEscaped() + QStringLiteral("</span>"),
        this);
    pathLabel->setTextFormat(Qt::RichText);
    pathLabel->setWordWrap(true);
    layout->addWidget(pathLabel);

    auto* modeLabel = new QLabel(
        currentlyCustom
            ? QStringLiteral("<span style=\"color:#5f6368;\">变更方式：</span><b style=\"color:#1f2328;\">独立密码</b><span style=\"color:#5f6368;\">  →  </span><b style=\"color:#0d47a1;\">主密码</b>")
            : QStringLiteral("<span style=\"color:#5f6368;\">变更方式：</span><b style=\"color:#1f2328;\">主密码</b><span style=\"color:#5f6368;\">  →  </span><b style=\"color:#0d47a1;\">独立密码</b>"),
        this);
    modeLabel->setTextFormat(Qt::RichText);
    layout->addWidget(modeLabel);

    auto* form = new QFormLayout();
    cur_edit_ = make_pw_edit(
        this, currentlyCustom ? QStringLiteral("请输入该目录当前的独立密码")
                              : QStringLiteral("请输入当前的主密码"));
    form->addRow(currentlyCustom ? QStringLiteral("当前独立密码:") : QStringLiteral("当前主密码:"), cur_edit_);

    if (target_custom_) {
        new_edit_ = make_pw_edit(this, QStringLiteral("至少 4 个字符"));
        confirm_edit_ = make_pw_edit(this, QStringLiteral("再次输入新独立密码"));
        form->addRow(QStringLiteral("新独立密码:"), new_edit_);
        form->addRow(QStringLiteral("确认新密码:"), confirm_edit_);
    } else {
        auto* note = new QLabel(QStringLiteral("切换后该目录将使用主密码解锁，原独立密码随即失效。"), this);
        note->setWordWrap(true);
        layout->addWidget(note);
    }
    layout->addLayout(form);

    if (target_custom_) {
        layout->addWidget(make_password_disclaimer(this, ack_cb_));
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(
        target_custom_ ? QStringLiteral("切换为独立密码") : QStringLiteral("切换为主密码"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ChangeDirPasswordDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString ChangeDirPasswordDialog::currentPassword() const { return cur_edit_->text(); }
bool ChangeDirPasswordDialog::useCustomPassword() const { return target_custom_; }
QString ChangeDirPasswordDialog::newCustomPassword() const { return new_edit_ ? new_edit_->text() : QString(); }

void ChangeDirPasswordDialog::onAccept() {
    if (cur_edit_->text().isEmpty()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("请先输入当前密码进行验证。"));
        cur_edit_->setFocus();
        return;
    }
    if (target_custom_) {
        if (new_edit_->text().size() < 4) {
            showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("新独立密码长度至少需要 4 个字符。"));
            new_edit_->setFocus();
            return;
        }
        if (new_edit_->text() != confirm_edit_->text()) {
            showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("两次输入的新独立密码不一致。"));
            confirm_edit_->selectAll();
            confirm_edit_->setFocus();
            return;
        }
        if (!ensure_disclaimer_checked(this, ack_cb_)) return;
    }
    accept();
}

EditCustomPasswordDialog::EditCustomPasswordDialog(const QString& dirPath, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("修改独立密码"));
    setMinimumWidth(470);

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* pathLabel = new QLabel(
        QStringLiteral("<span style=\"color:#5f6368;\">目标目录：</span><span style=\"color:#1f2328;\">") +
            dirPath.toHtmlEscaped() + QStringLiteral("</span>"),
        this);
    pathLabel->setTextFormat(Qt::RichText);
    pathLabel->setWordWrap(true);
    layout->addWidget(pathLabel);

    auto* modeLabel = new QLabel(
        QStringLiteral("<span style=\"color:#5f6368;\">当前方式：</span><b style=\"color:#0d47a1;\">独立密码</b><span style=\"color:#5f6368;\">（仅修改本目录的独立密码，不影响主密码）</span>"),
        this);
    modeLabel->setTextFormat(Qt::RichText);
    modeLabel->setWordWrap(true);
    layout->addWidget(modeLabel);

    auto* form = new QFormLayout();
    cur_edit_ = make_pw_edit(this, QStringLiteral("请输入当前独立密码"));
    new_edit_ = make_pw_edit(this, QStringLiteral("至少 4 个字符"));
    confirm_edit_ = make_pw_edit(this, QStringLiteral("再次输入新独立密码"));
    form->addRow(QStringLiteral("当前独立密码:"), cur_edit_);
    form->addRow(QStringLiteral("新独立密码:"), new_edit_);
    form->addRow(QStringLiteral("确认新密码:"), confirm_edit_);
    layout->addLayout(form);

    layout->addWidget(make_password_disclaimer(this, ack_cb_));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存新密码"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &EditCustomPasswordDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString EditCustomPasswordDialog::currentPassword() const { return cur_edit_->text(); }
QString EditCustomPasswordDialog::newCustomPassword() const { return new_edit_->text(); }

void EditCustomPasswordDialog::onAccept() {
    if (cur_edit_->text().isEmpty()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("请输入当前独立密码进行验证。"));
        cur_edit_->setFocus();
        return;
    }
    if (new_edit_->text().size() < 4) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("新独立密码长度至少需要 4 个字符。"));
        new_edit_->setFocus();
        return;
    }
    if (new_edit_->text() != confirm_edit_->text()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("两次输入的新密码不一致。"));
        confirm_edit_->selectAll();
        confirm_edit_->setFocus();
        return;
    }
    if (!ensure_disclaimer_checked(this, ack_cb_)) return;
    accept();
}

SettingsDialog::SettingsDialog(int autoRelockMinutes, bool relockOnScreenLock,
                               const QString& hotkey, bool hotkeyOk, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("安全与自动回锁设置"));
    setMinimumWidth(440);

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* form = new QFormLayout();

    minutes_spin_ = new QSpinBox(this);
    minutes_spin_->setRange(0, 1440);
    minutes_spin_->setValue(autoRelockMinutes);
    minutes_spin_->setSpecialValueText(QStringLiteral("已关闭 (0 分钟)"));
    minutes_spin_->setSuffix(QStringLiteral(" 分钟"));
    form->addRow(QStringLiteral("无操作自动回锁:"), minutes_spin_);

    screen_lock_cb_ = new QCheckBox(QStringLiteral("锁屏或系统休眠时自动锁定所有目录"), this);
    screen_lock_cb_->setChecked(relockOnScreenLock);
    form->addRow(QStringLiteral("锁屏联动:"), screen_lock_cb_);

    auto* hkLabel = new QLabel(
        hotkey + (hotkeyOk ? QStringLiteral("（全局快捷键已生效）")
                           : QStringLiteral("（当前会话未注册全局热键，可使用托盘一键锁定）")),
        this);
    hkLabel->setWordWrap(true);
    form->addRow(QStringLiteral("一键全锁热键:"), hkLabel);

    pw_edit_ = make_pw_edit(this, QStringLiteral("需验证主密码方可保存设置"));
    form->addRow(QStringLiteral("验证主密码:"), pw_edit_);
    layout->addLayout(form);

    auto* buttons = make_button_box(this, QStringLiteral("保存"), QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

int SettingsDialog::autoRelockMinutes() const { return minutes_spin_->value(); }
bool SettingsDialog::relockOnScreenLock() const { return screen_lock_cb_->isChecked(); }
QString SettingsDialog::masterPassword() const { return pw_edit_->text(); }

void SettingsDialog::onAccept() {
    if (pw_edit_->text().isEmpty()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("请输入主密码以确认修改。"));
        pw_edit_->setFocus();
        return;
    }
    accept();
}

VerifyMasterDialog::VerifyMasterDialog(const QString& title, const QString& prompt, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(title);
    setMinimumWidth(430);

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* promptLabel = new QLabel(prompt, this);
    promptLabel->setWordWrap(true);
    layout->addWidget(promptLabel);

    auto* form = new QFormLayout();
    pw_edit_ = make_pw_edit(this, QStringLiteral("请输入主密码"));
    form->addRow(QStringLiteral("主密码:"), pw_edit_);
    layout->addLayout(form);

    auto* buttons = make_button_box(this, QStringLiteral("验证"), QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &VerifyMasterDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    pw_edit_->setFocus();
    if (!parent) {
        QTimer::singleShot(0, this, [this]() {
            raise();
            activateWindow();
            pw_edit_->setFocus();
        });
    }
}

QString VerifyMasterDialog::password() const {
    return pw_edit_->text();
}

void VerifyMasterDialog::onAccept() {
    if (pw_edit_->text().isEmpty()) {
        showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"), QStringLiteral("请输入主密码。"));
        pw_edit_->setFocus();
        return;
    }
    accept();
}

FolderPickerDialog::FolderPickerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("选择要加锁的目录"));
    resize(720, 480);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // Top path bar
    auto* topBar = new QHBoxLayout();
    auto* upBtn = new QPushButton(QStringLiteral("↑ 上级"), this);
    upBtn->setMinimumWidth(72);
    path_edit_ = new QLineEdit(this);
    path_edit_->setPlaceholderText(QStringLiteral("输入或浏览目录路径..."));
    auto* goBtn = new QPushButton(QStringLiteral("转到"), this);
    goBtn->setMinimumWidth(64);
    topBar->addWidget(upBtn);
    topBar->addWidget(path_edit_, 1);
    topBar->addWidget(goBtn);
    root->addLayout(topBar);

    // Middle split: quick sidebar + folder view
    auto* midRow = new QHBoxLayout();
    auto* sidebar = new QListWidget(this);
    sidebar->setFixedWidth(164);

    auto addPlace = [&](const QString& label, const QString& p) {
        if (p.isEmpty() || !QFileInfo(p).isDir()) return;
        for (int i = 0; i < sidebar->count(); ++i) {
            if (sidebar->item(i)->data(Qt::UserRole).toString() == p) return;
        }
        auto* item = new QListWidgetItem(label, sidebar);
        item->setData(Qt::UserRole, p);
    };

    addPlace(QStringLiteral("🏠 主目录"), QDir::homePath());
    addPlace(QStringLiteral("🖥 桌面"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    addPlace(QStringLiteral("📄 文档"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    addPlace(QStringLiteral("📥 下载"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    addPlace(QStringLiteral("💻 根目录 (/)"), QStringLiteral("/"));

    for (const QString& baseRoot : {QStringLiteral("/media/") + qEnvironmentVariable("USER"),
                                    QStringLiteral("/media"),
                                    QStringLiteral("/mnt")}) {
        QDir md(baseRoot);
        if (!md.exists()) continue;
        for (const QFileInfo& sub : md.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            addPlace(QStringLiteral("💾 ") + sub.fileName(), sub.absoluteFilePath());
        }
    }

    fs_model_ = new QFileSystemModel(this);
    fs_model_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    fs_model_->setRootPath(QStringLiteral("/"));

    tree_view_ = new QTreeView(this);
    tree_view_->setModel(fs_model_);
    tree_view_->setHeaderHidden(true);
    tree_view_->setItemsExpandable(false);
    tree_view_->setRootIsDecorated(false);
    tree_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_view_->hideColumn(1);
    tree_view_->hideColumn(2);
    tree_view_->hideColumn(3);

    midRow->addWidget(sidebar);
    midRow->addWidget(tree_view_, 1);
    root->addLayout(midRow, 1);

    // Bottom readout + hint + Chinese OK/Cancel
    preview_label_ = new QLabel(this);
    preview_label_->setTextFormat(Qt::RichText);
    preview_label_->setWordWrap(true);
    root->addWidget(preview_label_);

    auto* hintLabel = new QLabel(
        QStringLiteral("<span style=\"color:#5f6368;\">提示：单击选中子目录，或双击进入目标目录后直接点击「确定」。</span>"),
        this);
    hintLabel->setTextFormat(Qt::RichText);
    root->addWidget(hintLabel);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    ok_btn_ = new QPushButton(QStringLiteral("确定"), this);
    ok_btn_->setMinimumWidth(88);
    ok_btn_->setAutoDefault(false);
    ok_btn_->setDefault(false);
    auto* cancelBtn = new QPushButton(QStringLiteral("取消"), this);
    cancelBtn->setMinimumWidth(88);
    cancelBtn->setAutoDefault(false);
    cancelBtn->setDefault(false);
    btnRow->addWidget(ok_btn_);
    btnRow->addWidget(cancelBtn);
    root->addLayout(btnRow);

    QString startDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (startDir.isEmpty() || !QFileInfo(startDir).isDir()) {
        startDir = QDir::homePath();
    }
    navigateTo(startDir);

    connect(upBtn, &QPushButton::clicked, this, [this]() {
        QDir d(current_dir_);
        if (d.cdUp()) navigateTo(d.absolutePath());
    });
    connect(goBtn, &QPushButton::clicked, this, [this]() {
        QString p = path_edit_->text().trimmed();
        if (QFileInfo(p).isDir()) {
            navigateTo(QDir(p).absolutePath());
        } else {
            showMessageZh(this, QMessageBox::Warning, QStringLiteral("提示"),
                          QStringLiteral("输入的目录不存在或不可访问。"));
        }
    });
    connect(path_edit_, &QLineEdit::returnPressed, goBtn, &QPushButton::click);

    connect(sidebar, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        QString p = item->data(Qt::UserRole).toString();
        if (QFileInfo(p).isDir()) navigateTo(p);
    });

    connect(tree_view_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection&, const QItemSelection&) {
        updateSelectionPreview();
    });
    connect(tree_view_, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (!idx.isValid()) return;
        QString sub = fs_model_->filePath(idx);
        if (QFileInfo(sub).isDir()) navigateTo(sub);
    });
    connect(ok_btn_, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void FolderPickerDialog::navigateTo(const QString& dirPath) {
    QDir d(dirPath);
    if (!d.exists()) return;
    current_dir_ = d.absolutePath();
    path_edit_->setText(current_dir_);
    QModelIndex idx = fs_model_->index(current_dir_);
    if (idx.isValid()) {
        tree_view_->setRootIndex(idx);
        tree_view_->clearSelection();
    }
    updateSelectionPreview();
}

void FolderPickerDialog::updateSelectionPreview() {
    auto sel = tree_view_->selectionModel() ? tree_view_->selectionModel()->selectedRows()
                                            : QModelIndexList();
    if (!sel.isEmpty()) {
        chosen_dir_ = fs_model_->filePath(sel.first());
    } else {
        chosen_dir_ = current_dir_;
    }
    preview_label_->setText(
        QStringLiteral("<span style=\"color:#5f6368;\">将锁定目录：</span><b style=\"color:#0d47a1;\">") +
        chosen_dir_.toHtmlEscaped() + QStringLiteral("</b>"));
    ok_btn_->setEnabled(!chosen_dir_.isEmpty());
}

QString FolderPickerDialog::selectedDirectory() const {
    return chosen_dir_;
}

}  // namespace dirlocker

