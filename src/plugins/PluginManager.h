#pragma once
#include <QObject>
#include <QList>
#include <memory>

namespace gitbolt::plugins {

class PluginInterface;

class PluginManager : public QObject {
    Q_OBJECT
public:
    explicit PluginManager(QObject* parent = nullptr);
    void loadPlugins(const QString& directory);
    QList<PluginInterface*> plugins() const;

private:
    QList<PluginInterface*> plugins_;
};

} // namespace gitbolt::plugins
