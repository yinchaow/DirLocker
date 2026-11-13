#include "../../core/json_mini.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QTranslator>

#include <unistd.h>

namespace {

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

