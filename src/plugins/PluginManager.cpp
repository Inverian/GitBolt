#include "plugins/PluginManager.h"
#include "plugins/PluginInterface.h"

#include <QCoreApplication>
#include <QDir>
#include <QPluginLoader>

namespace gitbolt::plugins {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PluginManager::PluginManager(PluginContext* context, QObject* parent)
    : QObject(parent)
    , context_(context)
    , settings_(QSettings::IniFormat, QSettings::UserScope,
                QCoreApplication::organizationName().isEmpty()
                    ? QStringLiteral("GitBolt")
                    : QCoreApplication::organizationName(),
                QCoreApplication::applicationName().isEmpty()
                    ? QStringLiteral("GitBolt")
                    : QCoreApplication::applicationName())
{
}

PluginManager::~PluginManager()
{
    // Shutdown all plugins in reverse order
    QList<QString> names = entries_.keys();
    for (auto it = names.crbegin(); it != names.crend(); ++it) {
        auto& entry = entries_[*it];
        if (entry.instance)
            entry.instance->shutdown();
        if (entry.loader) {
            entry.loader->unload();
            delete entry.loader;
        }
    }
    entries_.clear();
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void PluginManager::loadPlugins(const QString& directory)
{
    QDir dir(directory);
    if (!dir.exists())
        return;

    const QStringList filters = {
#if defined(Q_OS_WIN)
        QStringLiteral("*.dll"),
#elif defined(Q_OS_MACOS)
        QStringLiteral("*.dylib"),
        QStringLiteral("*.bundle"),
#else
        QStringLiteral("*.so"),
#endif
    };

    for (const QString& fileName : dir.entryList(filters, QDir::Files)) {
        loadPlugin(dir.absoluteFilePath(fileName));
    }
}

bool PluginManager::loadPlugin(const QString& path)
{
    auto* loader = new QPluginLoader(path, this);
    QObject* instance = loader->instance();
    if (!instance) {
        qWarning("PluginManager: failed to load %s — %s",
                 qPrintable(path), qPrintable(loader->errorString()));
        delete loader;
        return false;
    }

    auto* plugin = qobject_cast<PluginInterface*>(instance);
    if (!plugin) {
        qWarning("PluginManager: %s does not implement PluginInterface", qPrintable(path));
        loader->unload();
        delete loader;
        return false;
    }

    const QString name = plugin->name();
    if (entries_.contains(name)) {
        qWarning("PluginManager: duplicate plugin name '%s'", qPrintable(name));
        loader->unload();
        delete loader;
        return false;
    }

    bool enabled = settings_.value(QStringLiteral("plugins/%1/enabled").arg(name), true).toBool();

    PluginEntry entry;
    entry.instance = plugin;
    entry.loader = loader;
    entry.enabled = enabled;

    if (enabled) {
        if (!plugin->initialize(context_)) {
            qWarning("PluginManager: initialization failed for '%s'", qPrintable(name));
            loader->unload();
            delete loader;
            return false;
        }
    }

    entries_.insert(name, entry);
    emit pluginLoaded(name);
    return true;
}

// ---------------------------------------------------------------------------
// Unloading
// ---------------------------------------------------------------------------

bool PluginManager::unloadPlugin(const QString& name)
{
    auto it = entries_.find(name);
    if (it == entries_.end())
        return false;

    auto& entry = it.value();
    if (entry.instance)
        entry.instance->shutdown();

    if (entry.loader) {
        entry.loader->unload();
        delete entry.loader;
    }

    entries_.erase(it);
    emit pluginUnloaded(name);
    return true;
}

// ---------------------------------------------------------------------------
// Built-in plugins
// ---------------------------------------------------------------------------

void PluginManager::registerBuiltinPlugin(PluginInterface* plugin)
{
    if (!plugin)
        return;

    const QString name = plugin->name();
    if (entries_.contains(name))
        return;

    bool enabled = settings_.value(QStringLiteral("plugins/%1/enabled").arg(name), true).toBool();

    PluginEntry entry;
    entry.instance = plugin;
    entry.loader = nullptr;
    entry.enabled = enabled;

    if (enabled) {
        if (!plugin->initialize(context_)) {
            qWarning("PluginManager: built-in plugin '%s' failed to initialize", qPrintable(name));
            return;
        }
    }

    entries_.insert(name, entry);
    emit pluginLoaded(name);
}

// ---------------------------------------------------------------------------
// Enable / disable
// ---------------------------------------------------------------------------

void PluginManager::enablePlugin(const QString& name)
{
    auto it = entries_.find(name);
    if (it == entries_.end())
        return;

    auto& entry = it.value();
    if (entry.enabled)
        return;

    entry.enabled = true;
    settings_.setValue(QStringLiteral("plugins/%1/enabled").arg(name), true);

    if (entry.instance)
        entry.instance->initialize(context_);
}

void PluginManager::disablePlugin(const QString& name)
{
    auto it = entries_.find(name);
    if (it == entries_.end())
        return;

    auto& entry = it.value();
    if (!entry.enabled)
        return;

    if (entry.instance)
        entry.instance->shutdown();

    entry.enabled = false;
    settings_.setValue(QStringLiteral("plugins/%1/enabled").arg(name), false);
}

bool PluginManager::isPluginEnabled(const QString& name) const
{
    auto it = entries_.find(name);
    return it != entries_.end() && it->enabled;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

QList<PluginInterface*> PluginManager::plugins() const
{
    QList<PluginInterface*> result;
    result.reserve(entries_.size());
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it)
        result.append(it->instance);
    return result;
}

PluginInterface* PluginManager::pluginByName(const QString& name) const
{
    auto it = entries_.find(name);
    return it != entries_.end() ? it->instance : nullptr;
}

} // namespace gitbolt::plugins
