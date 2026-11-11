#pragma once

#include <QObject>
#include <menu-plugin-iface.h>

namespace dirlocker {

class DirLockerPeonyPlugin : public QObject, public Peony::MenuPluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MenuPluginInterface_iid)
    Q_INTERFACES(Peony::MenuPluginInterface)
public:
    explicit DirLockerPeonyPlugin(QObject* parent = nullptr);
    ~DirLockerPeonyPlugin() override = default;

    PluginType pluginType() override { return PluginType::MenuPlugin; }
    const QString name() override { return QStringLiteral("目录密匣"); }
    const QString description() override { return QStringLiteral("使用 目录密匣 锁定或解锁目录"); }
    const QIcon icon() override { return QIcon(QStringLiteral(":/icons/dirlocker-32.png")); }
    void setEnable(bool enable) override { enabled_ = enable; }
    bool isEnable() override { return enabled_; }

    QString testPlugin() override { return QStringLiteral("DirLocker Peony Menu Plugin"); }
    QList<QAction*> menuActions(Types types, const QString& uri,
                                const QStringList& selectionUris) override;

private:
    bool enabled_ = true;
};

}  // namespace dirlocker

