#pragma once

#include <QAction>
#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>
#include <QtPlugin>

namespace gitbolt::git {
class Repository;
} // namespace gitbolt::git

namespace gitbolt::conf {
class SettingsService;
} // namespace gitbolt::conf

namespace gitbolt::plugins {

// ---------------------------------------------------------------------------
// PluginContext — passed to each plugin during initialization so that it can
// interact with the host application without tight coupling.
// ---------------------------------------------------------------------------

class PluginContext {
public:
    virtual ~PluginContext() = default;

    /// Active repository (may be nullptr when no repo is open).
    virtual git::Repository* activeRepository() = 0;

    /// Register a QAction under a named menu (e.g. "Plugins").
    virtual void registerMenuItem(const QString& menu, QAction* action) = 0;

    /// Register a QAction on the main toolbar.
    virtual void registerToolBarAction(QAction* action) = 0;

    /// Show a transient notification / status-bar message.
    virtual void showNotification(const QString& message) = 0;

    /// Access the application settings service.
    virtual conf::SettingsService* settings() = 0;
};

// ---------------------------------------------------------------------------
// PluginInterface — the contract every plugin must implement.
// ---------------------------------------------------------------------------

class PluginInterface {
public:
    virtual ~PluginInterface() = default;

    /// Human-readable plugin name.
    virtual QString name() const = 0;

    /// Short description.
    virtual QString description() const = 0;

    /// Version string (e.g. "1.0.0").
    virtual QString version() const = 0;

    /// Called once after loading; return false to abort activation.
    virtual bool initialize(PluginContext* context) = 0;

    /// Called before unloading — release resources here.
    virtual void shutdown() = 0;

    /// Optional settings widget shown in the Settings dialog's plugin page.
    virtual QWidget* settingsWidget() { return nullptr; }

    /// Actions to be placed in the menu bar.
    virtual QList<QAction*> menuActions() { return {}; }

    /// Actions to be placed on the toolbar.
    virtual QList<QAction*> toolBarActions() { return {}; }
};

} // namespace gitbolt::plugins

Q_DECLARE_INTERFACE(gitbolt::plugins::PluginInterface, "com.gitbolt.PluginInterface/1.0")
