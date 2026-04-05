#pragma once

#include "plugins/PluginInterface.h"

#include <QAction>
#include <QObject>

namespace gitbolt::plugins {

/// Built-in plugin that provides a "Statistics" menu action.
/// When triggered it opens a dialog showing commit-per-author bar chart,
/// commits-over-time line chart, totals, and top changed files.
class StatisticsPlugin : public QObject, public PluginInterface {
    Q_OBJECT
    Q_INTERFACES(gitbolt::plugins::PluginInterface)
public:
    explicit StatisticsPlugin(QObject* parent = nullptr);
    ~StatisticsPlugin() override = default;

    // PluginInterface
    QString name() const override;
    QString description() const override;
    QString version() const override;
    bool initialize(PluginContext* context) override;
    void shutdown() override;
    QList<QAction*> menuActions() override;

private slots:
    void showStatistics();

private:
    PluginContext* ctx_ = nullptr;
    QAction* action_ = nullptr;
};

} // namespace gitbolt::plugins
