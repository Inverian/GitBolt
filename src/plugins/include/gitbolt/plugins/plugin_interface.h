#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QAction>

namespace gitbolt::plugins {

class PluginInterface {
public:
    virtual ~PluginInterface() = default;
    virtual QString name() const = 0;
    virtual QString description() const = 0;
    virtual QString version() const = 0;
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual QList<QAction*> menuActions() { return {}; }
};

} // namespace gitbolt::plugins

Q_DECLARE_INTERFACE(gitbolt::plugins::PluginInterface, "com.gitbolt.PluginInterface/1.0")
