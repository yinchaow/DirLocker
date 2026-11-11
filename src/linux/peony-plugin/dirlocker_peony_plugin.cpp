#include "dirlocker_peony_plugin.hpp"

#include "../gui/daemon_client.hpp"

#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace dirlocker {

DirLockerPeonyPlugin::DirLockerPeonyPlugin(QObject* parent) : QObject(parent) {}

QList<QAction*> DirLockerPeonyPlugin::menuActions(Types types, const QString&,
                                                  const QStringList& selectionUris) {
    if (!enabled_) return {};
    if (!types.testFlag(DirectoryView) && !types.testFlag(DesktopWindow)) return {};
    if (selectionUris.size() != 1) return {};

    QUrl u(selectionUris.first());
    if (!u.isLocalFile()) return {};
    QString localPath = u.toLocalFile();
    if (localPath.isEmpty()) return {};

    QFileInfo fi(localPath);
    const bool isStubFile = fi.isFile() && !fi.isSymLink() &&
                            (localPath.endsWith(QStringLiteral(".desktop"), Qt::CaseInsensitive) ||
                             localPath.endsWith(QStringLiteral(".dlock"), Qt::CaseInsensitive));
    const bool isRealDir = fi.exists() && fi.isDir() && !fi.isSymLink();
    if (!fi.exists() || (!isRealDir && !isStubFile)) return {};

    JsonValue qp = JsonValue::object();
    qp.set("path", localPath.toStdString());
    auto reply = DaemonClient::send("query", std::move(qp),
                                    DaemonClient::kDefaultSocketPath, 150);

    QString label;
    QString flag;
    QString targetArg = localPath;
    QIcon actIcon(QStringLiteral(":/icons/dirlocker-32.png"));

    if (reply.transport_ok && reply.ok) {
        bool protectable = reply.data.get("protectable").as_bool(false);
        bool managed = reply.data.get("managed").as_bool(false);
        if (isStubFile && !managed) return {};
        if (!protectable) return {};

        QString canon = QString::fromStdString(reply.data.get("canonical_path").as_string());
        if (!canon.isEmpty()) targetArg = canon;

        bool locked = reply.data.get("locked").as_bool(false);
        if (managed && locked) {
            label = QStringLiteral("目录密匣：输入密码解锁并打开");
            flag = QStringLiteral("--unlock");
            actIcon = QIcon(QStringLiteral(":/icons/dirlocker-folder-32.png"));
        } else {
            label = QStringLiteral("目录密匣：锁定此目录");
            flag = QStringLiteral("--lock");
            actIcon = QIcon(QStringLiteral(":/icons/dirlocker-32.png"));
        }
    } else {
        if (isStubFile) return {};
        label = QStringLiteral("目录密匣：锁定此目录");
        flag = QStringLiteral("--toggle");
    }

    auto* action = new QAction(actIcon, label, nullptr);
    QObject::connect(action, &QAction::triggered, [flag, targetArg]() {
        QProcess::startDetached(QStringLiteral("/usr/bin/dirlocker"), {flag, targetArg});
    });
    return {action};
}

}  // namespace dirlocker

