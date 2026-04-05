#include "plugins/PluginManager.h"
#include "plugins/PluginInterface.h"
#include <QDir>
#include <QPluginLoader>

namespace gitbolt::plugins {

PluginManager::PluginManager(QObject* parent) : QObject(parent) {}

void PluginManager::loadPlugins(const QString& directory) {
    QDir dir(directory);
    for (const auto& fileName : dir.entryList(QDir::Files)) {
        QPluginLoader loader(dir.absoluteFilePath(fileName));
        QObject* plugin = loader.instance();
        if (plugin) {
            auto* iface = qobject_cast<PluginInterface*>(plugin);
            if (iface && iface->initialize()) plugins_.append(iface);
        }
    }
}

QList<PluginInterface*> PluginManager::plugins() const { return plugins_; }

} // namespace gitbolt::plugins
