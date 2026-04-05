#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>

class QPluginLoader;

namespace gitbolt::plugins {

class PluginContext;
class PluginInterface;

class PluginManager : public QObject {
    Q_OBJECT
public:
    explicit PluginManager(PluginContext* context, QObject* parent = nullptr);
    ~PluginManager() override;

    /// Scan a directory for shared-library plugins and load them all.
    void loadPlugins(const QString& directory);

    /// Load a single plugin from a shared library path.
    bool loadPlugin(const QString& path);

    /// Unload (shutdown + release) a plugin by name.
    bool unloadPlugin(const QString& name);

    /// Register a built-in plugin that is not backed by a shared library.
    void registerBuiltinPlugin(PluginInterface* plugin);

    /// Enable / disable a plugin (persisted via QSettings).
    void enablePlugin(const QString& name);
    void disablePlugin(const QString& name);
    bool isPluginEnabled(const QString& name) const;

    /// List of all loaded plugin interfaces.
    QList<PluginInterface*> plugins() const;

    /// Look up a specific plugin by its name.
    PluginInterface* pluginByName(const QString& name) const;

signals:
    void pluginLoaded(const QString& name);
    void pluginUnloaded(const QString& name);

private:
    struct PluginEntry {
        PluginInterface* instance = nullptr;
        QPluginLoader* loader = nullptr; // nullptr for built-in plugins
        bool enabled = true;
    };

    PluginContext* context_ = nullptr;
    QHash<QString, PluginEntry> entries_;
    QSettings settings_;
};

} // namespace gitbolt::plugins
