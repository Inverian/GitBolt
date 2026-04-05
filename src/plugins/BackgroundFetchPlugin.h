#pragma once

#include "plugins/PluginInterface.h"

#include <QAction>
#include <QObject>
#include <QTimer>

class QCheckBox;
class QSpinBox;
class QWidget;

namespace gitbolt::plugins {

/// Built-in plugin that periodically fetches all remotes and notifies
/// the user when the remote tracking refs have advanced.
class BackgroundFetchPlugin : public QObject, public PluginInterface {
    Q_OBJECT
    Q_INTERFACES(gitbolt::plugins::PluginInterface)
public:
    explicit BackgroundFetchPlugin(QObject* parent = nullptr);
    ~BackgroundFetchPlugin() override = default;

    // PluginInterface
    QString name() const override;
    QString description() const override;
    QString version() const override;
    bool initialize(PluginContext* context) override;
    void shutdown() override;
    QWidget* settingsWidget() override;

private slots:
    void performFetch();

private:
    /// Collect remote tracking ref tips so we can compare before/after.
    QHash<QString, QString> collectRemoteRefs() const;

    PluginContext* ctx_ = nullptr;
    QTimer timer_;
    int intervalMinutes_ = 5;
    bool enabled_ = true;

    // Settings widget (lazy-created)
    QWidget* settingsWidget_ = nullptr;
    QCheckBox* enabledCheck_ = nullptr;
    QSpinBox* intervalSpin_ = nullptr;
};

} // namespace gitbolt::plugins
