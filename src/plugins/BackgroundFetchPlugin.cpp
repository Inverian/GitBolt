#include "plugins/BackgroundFetchPlugin.h"
#include "conf/SettingsService.h"
#include "git/Repository.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace gitbolt::plugins {

BackgroundFetchPlugin::BackgroundFetchPlugin(QObject* parent)
    : QObject(parent)
{
    connect(&timer_, &QTimer::timeout, this, &BackgroundFetchPlugin::performFetch);
}

QString BackgroundFetchPlugin::name() const
{
    return QStringLiteral("Background Fetch");
}

QString BackgroundFetchPlugin::description() const
{
    return tr("Periodically fetches all remotes and notifies when new commits arrive.");
}

QString BackgroundFetchPlugin::version() const
{
    return QStringLiteral("1.0.0");
}

bool BackgroundFetchPlugin::initialize(PluginContext* context)
{
    ctx_ = context;

    if (ctx_ && ctx_->settings()) {
        intervalMinutes_ = ctx_->settings()->value(
            QStringLiteral("plugins/BackgroundFetch/interval"), 5).toInt();
        enabled_ = ctx_->settings()->value(
            QStringLiteral("plugins/BackgroundFetch/enabled"), true).toBool();
    }

    if (enabled_) {
        timer_.setInterval(intervalMinutes_ * 60 * 1000);
        timer_.start();
    }

    return true;
}

void BackgroundFetchPlugin::shutdown()
{
    timer_.stop();

    if (ctx_ && ctx_->settings()) {
        ctx_->settings()->setValue(QStringLiteral("plugins/BackgroundFetch/interval"), intervalMinutes_);
        ctx_->settings()->setValue(QStringLiteral("plugins/BackgroundFetch/enabled"), enabled_);
    }

    ctx_ = nullptr;
}

QWidget* BackgroundFetchPlugin::settingsWidget()
{
    if (settingsWidget_)
        return settingsWidget_;

    settingsWidget_ = new QWidget;
    auto* layout = new QVBoxLayout(settingsWidget_);

    enabledCheck_ = new QCheckBox(tr("Enable background fetch"));
    enabledCheck_->setChecked(enabled_);
    layout->addWidget(enabledCheck_);

    auto* form = new QFormLayout;
    intervalSpin_ = new QSpinBox;
    intervalSpin_->setRange(1, 120);
    intervalSpin_->setSuffix(tr(" min"));
    intervalSpin_->setValue(intervalMinutes_);
    form->addRow(tr("Fetch interval:"), intervalSpin_);
    layout->addLayout(form);
    layout->addStretch();

    QObject::connect(enabledCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        enabled_ = checked;
        if (enabled_) {
            timer_.setInterval(intervalMinutes_ * 60 * 1000);
            timer_.start();
        } else {
            timer_.stop();
        }
    });

    QObject::connect(intervalSpin_, &QSpinBox::valueChanged, this, [this](int val) {
        intervalMinutes_ = val;
        if (enabled_) {
            timer_.setInterval(intervalMinutes_ * 60 * 1000);
            timer_.start(); // restart with new interval
        }
    });

    return settingsWidget_;
}

// ---------------------------------------------------------------------------
// Fetch logic
// ---------------------------------------------------------------------------

QHash<QString, QString> BackgroundFetchPlugin::collectRemoteRefs() const
{
    QHash<QString, QString> refs;
    if (!ctx_ || !ctx_->activeRepository())
        return refs;

    auto* repo = ctx_->activeRepository();
    auto branchesResult = repo->branches(git::BranchType::Remote);
    if (!branchesResult)
        return refs;

    for (const auto& b : branchesResult.value()) {
        refs.insert(QString::fromStdString(b.fullRefName),
                    QString::fromStdString(b.tipId.toHex()));
    }
    return refs;
}

void BackgroundFetchPlugin::performFetch()
{
    if (!ctx_ || !ctx_->activeRepository())
        return;

    auto* repo = ctx_->activeRepository();

    // Snapshot refs before fetch
    QHash<QString, QString> before = collectRemoteRefs();

    // Fetch all remotes via the CLI process helper
    auto process = repo->process();
    auto result = process.fetch(/*remote=*/"", /*prune=*/false);
    if (!result) {
        // Silently ignore fetch errors in background
        return;
    }

    // Snapshot refs after fetch
    QHash<QString, QString> after = collectRemoteRefs();

    // Compare
    int newCommitRefs = 0;
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        auto beforeIt = before.find(it.key());
        if (beforeIt == before.end() || beforeIt.value() != it.value())
            ++newCommitRefs;
    }

    if (newCommitRefs > 0 && ctx_) {
        ctx_->showNotification(
            tr("Background fetch: %n remote ref(s) updated.", "", newCommitRefs));
    }
}

} // namespace gitbolt::plugins
