#include "widgets/MaintenanceWidget.h"
#include "widgets/ConsoleOutputWidget.h"
#include "services/GitService.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace gitbolt::widgets {

MaintenanceWidget::MaintenanceWidget(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void MaintenanceWidget::setGitService(services::GitService* service) {
    if (gitService_) {
        disconnect(gitService_, nullptr, this, nullptr);
    }
    gitService_ = service;
    if (gitService_) {
        connect(gitService_, &services::GitService::maintenanceComplete,
                this, &MaintenanceWidget::onMaintenanceComplete);
    }
    refreshStats();
}

void MaintenanceWidget::refreshStats() {
    if (!gitService_ || !gitService_->isOpen()) {
        statsLabel_->setText(tr("No repository open."));
        return;
    }
    QString usage = gitService_->repositoryDiskUsage();
    statsLabel_->setText(usage);
    comparisonLabel_->clear();
}

void MaintenanceWidget::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);

    // Repository stats
    auto* statsGroup = new QGroupBox(tr("Repository Statistics"));
    auto* statsLayout = new QVBoxLayout(statsGroup);
    statsLabel_ = new QLabel(tr("No repository open."));
    statsLabel_->setWordWrap(true);
    statsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsLayout->addWidget(statsLabel_);
    mainLayout->addWidget(statsGroup);

    // Maintenance buttons
    auto* actionsGroup = new QGroupBox(tr("Maintenance Actions"));
    auto* actionsLayout = new QVBoxLayout(actionsGroup);

    auto* row1 = new QHBoxLayout;
    gcBtn_ = new QPushButton(tr("Garbage Collect"));
    pruneBtn_ = new QPushButton(tr("Prune"));
    row1->addWidget(gcBtn_);
    row1->addWidget(pruneBtn_);
    actionsLayout->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    fsckBtn_ = new QPushButton(tr("Fsck"));
    repackBtn_ = new QPushButton(tr("Repack"));
    row2->addWidget(fsckBtn_);
    row2->addWidget(repackBtn_);
    actionsLayout->addLayout(row2);

    runAllBtn_ = new QPushButton(tr("Run All (GC + Prune + Repack)"));
    actionsLayout->addWidget(runAllBtn_);

    mainLayout->addWidget(actionsGroup);

    // Before/after comparison
    comparisonLabel_ = new QLabel;
    comparisonLabel_->setWordWrap(true);
    mainLayout->addWidget(comparisonLabel_);

    // Console output
    console_ = new ConsoleOutputWidget;
    mainLayout->addWidget(console_);

    connect(gcBtn_, &QPushButton::clicked, this, &MaintenanceWidget::onGarbageCollect);
    connect(pruneBtn_, &QPushButton::clicked, this, &MaintenanceWidget::onPrune);
    connect(fsckBtn_, &QPushButton::clicked, this, &MaintenanceWidget::onFsck);
    connect(repackBtn_, &QPushButton::clicked, this, &MaintenanceWidget::onRepack);
    connect(runAllBtn_, &QPushButton::clicked, this, &MaintenanceWidget::onRunAll);
}

void MaintenanceWidget::setButtonsEnabled(bool enabled) {
    gcBtn_->setEnabled(enabled);
    pruneBtn_->setEnabled(enabled);
    fsckBtn_->setEnabled(enabled);
    repackBtn_->setEnabled(enabled);
    runAllBtn_->setEnabled(enabled);
}

void MaintenanceWidget::captureBeforeDiskUsage() {
    if (gitService_)
        beforeDiskUsage_ = gitService_->repositoryDiskUsage();
}

void MaintenanceWidget::onGarbageCollect() {
    if (!gitService_) return;
    auto answer = QMessageBox::question(this, tr("Garbage Collect"),
        tr("Run git gc? This will optimize the repository."));
    if (answer != QMessageBox::Yes) return;

    captureBeforeDiskUsage();
    setButtonsEnabled(false);
    console_->appendOutput(tr("Running garbage collection..."));
    gitService_->runGc();
}

void MaintenanceWidget::onPrune() {
    if (!gitService_) return;
    auto answer = QMessageBox::question(this, tr("Prune"),
        tr("Run git prune? This will remove unreachable objects."));
    if (answer != QMessageBox::Yes) return;

    captureBeforeDiskUsage();
    setButtonsEnabled(false);
    console_->appendOutput(tr("Running prune..."));
    gitService_->runPrune();
}

void MaintenanceWidget::onFsck() {
    if (!gitService_) return;
    setButtonsEnabled(false);
    console_->appendOutput(tr("Running fsck..."));
    gitService_->runFsck();
}

void MaintenanceWidget::onRepack() {
    if (!gitService_) return;
    auto answer = QMessageBox::question(this, tr("Repack"),
        tr("Run git repack? This will repack objects for efficiency."));
    if (answer != QMessageBox::Yes) return;

    captureBeforeDiskUsage();
    setButtonsEnabled(false);
    console_->appendOutput(tr("Running repack..."));
    gitService_->runRepack();
}

void MaintenanceWidget::onRunAll() {
    if (!gitService_) return;
    auto answer = QMessageBox::question(this, tr("Run All Maintenance"),
        tr("Run gc, prune, and repack in sequence?"));
    if (answer != QMessageBox::Yes) return;

    captureBeforeDiskUsage();
    setButtonsEnabled(false);
    console_->clear();
    runAllStep_ = 1;
    console_->appendOutput(tr("Step 1/3: Running garbage collection..."));
    gitService_->runGc();
}

void MaintenanceWidget::onMaintenanceComplete(const QString& output) {
    console_->appendOutput(output);

    // Handle run-all sequence
    if (runAllStep_ > 0) {
        runAllStep_++;
        if (runAllStep_ == 2) {
            console_->appendOutput(tr("Step 2/3: Running prune..."));
            gitService_->runPrune();
            return;
        } else if (runAllStep_ == 3) {
            console_->appendOutput(tr("Step 3/3: Running repack..."));
            gitService_->runRepack();
            return;
        } else {
            runAllStep_ = 0;
            console_->appendOutput(tr("All maintenance operations complete."));
        }
    }

    setButtonsEnabled(true);

    // Show before/after comparison
    if (!beforeDiskUsage_.isEmpty() && gitService_) {
        QString afterUsage = gitService_->repositoryDiskUsage();
        comparisonLabel_->setText(
            tr("Before:\n%1\nAfter:\n%2").arg(beforeDiskUsage_, afterUsage));
        beforeDiskUsage_.clear();
        refreshStats();
    }
}

} // namespace gitbolt::widgets
