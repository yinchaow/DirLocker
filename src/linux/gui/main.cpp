#include "../../core/json_mini.hpp"
#include "../../core/version.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPushButton>
#include <QTimer>
#include <QTranslator>
#include <QVBoxLayout>

#include <unistd.h>

namespace {

bool showPostInstallDialog() {
    QDialog dlg;
    dlg.setWindowTitle(QStringLiteral("目录密匣 安装完成"));
    dlg.setWindowIcon(QIcon(QStringLiteral(":/icons/dirlocker-64.png")));
    dlg.setFixedWidth(440);
    dlg.setStyleSheet(QStringLiteral(
        "QDialog { background: #eef3fa; }"
        "QFrame#postCard { background: #ffffff; border: 1px solid #d8e1ec; border-radius: 12px; }"
        "QCheckBox { color: #0f172a; font-size: 10pt; font-weight: 500; spacing: 8px; }"
        "QPushButton#finishBtn { background: #0067c0; color: #ffffff; border: none; "
        "  border-radius: 8px; padding: 7px 24px; font-weight: 600; }"
        "QPushButton#finishBtn:hover { background: #005fb8; }"
        "QPushButton#finishBtn:pressed { background: #00529e; }"));

    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(20, 20, 20, 18);
    root->setSpacing(14);

    auto* card = new QFrame(&dlg);
    card->setObjectName(QStringLiteral("postCard"));
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(18, 18, 18, 16);
    cardLay->setSpacing(14);

    auto* header = new QHBoxLayout();
    header->setSpacing(14);
    auto* iconLbl = new QLabel(card);
    iconLbl->setPixmap(QIcon(QStringLiteral(":/icons/dirlocker-64.png")).pixmap(48, 48));
    iconLbl->setFixedSize(48, 48);
    header->addWidget(iconLbl, 0, Qt::AlignTop);

    auto* titleCol = new QVBoxLayout();
    titleCol->setSpacing(4);
    auto* titleLbl = new QLabel(
        QStringLiteral("<span style=\"font-size:13pt; font-weight:700; color:#0f172a;\">目录密匣</span> "
                       "<span style=\"background:#e8f3fc; color:#0067c0; font-size:9pt; font-weight:600; "
                       "padding:1px 7px; border-radius:8px;\">&nbsp;v" DIRLOCKER_VERSION_STR "&nbsp;</span>"
                       "<span style=\"font-size:12pt; font-weight:600; color:#0f172a;\"> 已安装完成</span>"),
        card);
    titleLbl->setTextFormat(Qt::RichText);
    auto* subLbl = new QLabel(
        QStringLiteral("后台保护服务已就绪并设为开机自启，支持右键或拖入目录快速加锁。"), card);
    subLbl->setStyleSheet(QStringLiteral("color: #475569; font-size: 9.5pt;"));
    subLbl->setWordWrap(true);
    titleCol->addWidget(titleLbl);
    titleCol->addWidget(subLbl);
    header->addLayout(titleCol, 1);
    cardLay->addLayout(header);

    auto* div = new QFrame(card);
    div->setFrameShape(QFrame::HLine);
    div->setStyleSheet(QStringLiteral("color: #e2e8f0;"));
    cardLay->addWidget(div);

    auto* launchCb = new QCheckBox(QStringLiteral("立即启动 目录密匣"), card);
    launchCb->setChecked(true);
    cardLay->addWidget(launchCb);
    root->addWidget(card);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto* finishBtn = new QPushButton(QStringLiteral("完成"), &dlg);
    finishBtn->setObjectName(QStringLiteral("finishBtn"));
    finishBtn->setDefault(true);
    QObject::connect(finishBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(finishBtn);
    root->addLayout(btnRow);

    if (dlg.exec() == QDialog::Accepted && launchCb->isChecked()) {
        return true;
    }
    return false;
}

QString singleInstanceName() {
    return QStringLiteral("dirlocker_gui_uid_%1").arg(static_cast<unsigned long>(getuid()));
}

void parseArgs(const QStringList& args, QString& action, QString& path, bool& trayOnly) {
    action = QStringLiteral("show");
    path.clear();
    trayOnly = false;

    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args[i];
        if (a == "--tray") {
            trayOnly = true;
            action = QStringLiteral("none");
        } else if (a == "--post-install") {
            action = QStringLiteral("post-install");
        } else if (a == "--lock" && i + 1 < args.size()) {
            action = QStringLiteral("lock");
            path = args[++i];
        } else if (a == "--unlock" && i + 1 < args.size()) {
            action = QStringLiteral("unlock");
            path = args[++i];
        } else if (a == "--unlock-id" && i + 1 < args.size()) {
            action = QStringLiteral("unlock-id");
            path = args[++i];
        } else if (a == "--toggle" && i + 1 < args.size()) {
            action = QStringLiteral("toggle");
            path = args[++i];
        } else if (a == "--lock-all") {
            action = QStringLiteral("lock-all");
        } else if (a == "--quit-silent" || a == "--quit") {
            action = QStringLiteral("quit-silent");
        } else if (a == "--show") {
            action = QStringLiteral("show");
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DirLocker"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/dirlocker-64.png")));
    app.setQuitOnLastWindowClosed(false);
    app.setStyleSheet(QStringLiteral(
        "QDialog { background: #f2f6fc; color: #0f172a; }"
        "QLineEdit, QSpinBox { background: #ffffff; border: 1px solid #cbd5e1; border-radius: 7px; padding: 4px 8px; color: #0f172a; }"
        "QLineEdit:focus, QSpinBox:focus { border: 1px solid #0067c0; }"
        "QDialogButtonBox QPushButton { background: #ffffff; border: 1px solid #cbd5e1; border-radius: 7px; padding: 5px 16px; color: #1e293b; }"
        "QDialogButtonBox QPushButton:hover { background: #f8fafc; border-color: #0067c0; color: #0067c0; }"
        "QMenu { background: #ffffff; border: 1px solid #d8e1ec; border-radius: 8px; padding: 4px; }"
        "QMenu::item { padding: 6px 20px 6px 12px; border-radius: 6px; color: #1e293b; }"
        "QMenu::item:selected { background: #e8f3fc; color: #0067c0; }"
        "QMenu::separator { height: 1px; background: #e2e8f0; margin: 4px 8px; }"));

    // Best-effort loading of Qt built-in zh_CN strings (e.g., QLineEdit context menus)
    QTranslator* qtBaseTr = new QTranslator(&app);
    const QString qtTrDir = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
    if (qtBaseTr->load(QStringLiteral("qtbase_zh_CN"), qtTrDir) ||
        qtBaseTr->load(QStringLiteral("qt_zh_CN"), qtTrDir) ||
        qtBaseTr->load(QStringLiteral("qtbase_zh_CN"), QStringLiteral("/usr/share/qt5/translations")) ||
        qtBaseTr->load(QStringLiteral("qt_zh_CN"), QStringLiteral("/usr/share/qt5/translations"))) {
        app.installTranslator(qtBaseTr);
    }

    QString action;
    QString path;
    bool trayOnly = false;
    parseArgs(app.arguments(), action, path, trayOnly);

    if (action == QStringLiteral("post-install")) {
        if (!showPostInstallDialog()) {
            return 0;
        }
        action = QStringLiteral("show");
    }

    const QString serverName = singleInstanceName();
    {
        QLocalSocket probe;
        probe.connectToServer(serverName);
        if (probe.waitForConnected(400)) {
            dirlocker::JsonValue msg = dirlocker::JsonValue::object();
            msg.set("action", (action == "none" ? "show" : action.toStdString()));
            msg.set("path", path.toStdString());
            std::string raw = msg.dump(-1) + "\n";
            probe.write(raw.data(), static_cast<qint64>(raw.size()));
            probe.waitForBytesWritten(1000);
            probe.disconnectFromServer();
            return 0;
        }
    }

    QLocalServer::removeServer(serverName);
    QLocalServer ipcServer;
    ipcServer.listen(serverName);

    dirlocker::MainWindow win;
    if (!trayOnly && action == "show") {
        win.showMainWindow();
    }

    QObject::connect(&ipcServer, &QLocalServer::newConnection, [&]() {
        while (auto* sock = ipcServer.nextPendingConnection()) {
            QObject::connect(sock, &QLocalSocket::readyRead, [sock, &win]() {
                QByteArray data = sock->readAll();
                dirlocker::JsonValue msg;
                if (dirlocker::JsonValue::parse(data.trimmed().toStdString(), msg) && msg.is_object()) {
                    QString a = QString::fromStdString(msg.get("action").as_string_or("show"));
                    QString p = QString::fromStdString(msg.get("path").as_string());
                    QTimer::singleShot(0, &win, [&win, a, p]() {
                        win.handleExternalCommand(a, p);
                    });
                }
                sock->disconnectFromServer();
                sock->deleteLater();
            });
            QObject::connect(sock, &QLocalSocket::disconnected, sock, &QLocalSocket::deleteLater);
        }
    });

    if (action != "none" && action != "show") {
        QTimer::singleShot(0, &win, [&win, action, path]() {
            win.handleExternalCommand(action, path);
        });
    }

    return app.exec();
}

