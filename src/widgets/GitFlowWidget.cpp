#include "widgets/GitFlowWidget.h"
#include "widgets/ConsoleOutputWidget.h"
#include "services/GitService.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace gitbolt::widgets {

GitFlowWidget::GitFlowWidget(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void GitFlowWidget::setGitService(services::GitService* service) {
    if (gitService_) {
        disconnect(gitService_, nullptr, this, nullptr);
    }
    gitService_ = service;
    if (gitService_) {
        connect(gitService_, &services::GitService::gitFlowOperationComplete,
                this, &GitFlowWidget::onGitFlowOperationComplete);
    }
    updateUiState();
}

void GitFlowWidget::refresh() {
    if (!gitService_ || !gitService_->isOpen()) return;

    updateUiState();

    if (gitService_->isGitFlowInitialized()) {
        featureList_->clear();
        featureList_->addItems(gitService_->activeFeatures());

        releaseList_->clear();
        releaseList_->addItems(gitService_->activeReleases());

        hotfixList_->clear();
        hotfixList_->addItems(gitService_->activeHotfixes());
    }
}

void GitFlowWidget::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);

    // Status bar
    auto* statusLayout = new QHBoxLayout;
    statusLabel_ = new QLabel(tr("Git Flow: Not initialized"));
    initBtn_ = new QPushButton(tr("Initialize Git Flow"));
    statusLayout->addWidget(statusLabel_);
    statusLayout->addStretch();
    statusLayout->addWidget(initBtn_);
    mainLayout->addLayout(statusLayout);

    connect(initBtn_, &QPushButton::clicked, this, &GitFlowWidget::onInitializeGitFlow);

    // Features
    featureGroup_ = new QGroupBox(tr("Features"));
    auto* featureLayout = new QVBoxLayout(featureGroup_);
    featureList_ = new QListWidget;
    auto* featureBtnLayout = new QHBoxLayout;
    startFeatureBtn_ = new QPushButton(tr("Start Feature"));
    finishFeatureBtn_ = new QPushButton(tr("Finish Feature"));
    featureBtnLayout->addWidget(startFeatureBtn_);
    featureBtnLayout->addWidget(finishFeatureBtn_);
    featureLayout->addWidget(featureList_);
    featureLayout->addLayout(featureBtnLayout);
    mainLayout->addWidget(featureGroup_);

    connect(startFeatureBtn_, &QPushButton::clicked, this, &GitFlowWidget::onStartFeature);
    connect(finishFeatureBtn_, &QPushButton::clicked, this, &GitFlowWidget::onFinishFeature);

    // Releases
    releaseGroup_ = new QGroupBox(tr("Releases"));
    auto* releaseLayout = new QVBoxLayout(releaseGroup_);
    releaseList_ = new QListWidget;
    auto* releaseBtnLayout = new QHBoxLayout;
    startReleaseBtn_ = new QPushButton(tr("Start Release"));
    finishReleaseBtn_ = new QPushButton(tr("Finish Release"));
    releaseBtnLayout->addWidget(startReleaseBtn_);
    releaseBtnLayout->addWidget(finishReleaseBtn_);
    releaseLayout->addWidget(releaseList_);
    releaseLayout->addLayout(releaseBtnLayout);
    mainLayout->addWidget(releaseGroup_);

    connect(startReleaseBtn_, &QPushButton::clicked, this, &GitFlowWidget::onStartRelease);
    connect(finishReleaseBtn_, &QPushButton::clicked, this, &GitFlowWidget::onFinishRelease);

    // Hotfixes
    hotfixGroup_ = new QGroupBox(tr("Hotfixes"));
    auto* hotfixLayout = new QVBoxLayout(hotfixGroup_);
    hotfixList_ = new QListWidget;
    auto* hotfixBtnLayout = new QHBoxLayout;
    startHotfixBtn_ = new QPushButton(tr("Start Hotfix"));
    finishHotfixBtn_ = new QPushButton(tr("Finish Hotfix"));
    hotfixBtnLayout->addWidget(startHotfixBtn_);
    hotfixBtnLayout->addWidget(finishHotfixBtn_);
    hotfixLayout->addWidget(hotfixList_);
    hotfixLayout->addLayout(hotfixBtnLayout);
    mainLayout->addWidget(hotfixGroup_);

    connect(startHotfixBtn_, &QPushButton::clicked, this, &GitFlowWidget::onStartHotfix);
    connect(finishHotfixBtn_, &QPushButton::clicked, this, &GitFlowWidget::onFinishHotfix);

    // Console output
    console_ = new ConsoleOutputWidget;
    mainLayout->addWidget(console_);

    updateUiState();
}

void GitFlowWidget::updateUiState() {
    bool initialized = gitService_ && gitService_->isOpen()
                       && gitService_->isGitFlowInitialized();

    initBtn_->setVisible(!initialized);
    statusLabel_->setText(initialized ? tr("Git Flow: Initialized")
                                      : tr("Git Flow: Not initialized"));

    featureGroup_->setEnabled(initialized);
    releaseGroup_->setEnabled(initialized);
    hotfixGroup_->setEnabled(initialized);

    finishFeatureBtn_->setEnabled(initialized && featureList_->currentItem() != nullptr);
    finishReleaseBtn_->setEnabled(initialized && releaseList_->currentItem() != nullptr);
    finishHotfixBtn_->setEnabled(initialized && hotfixList_->currentItem() != nullptr);
}

void GitFlowWidget::onInitializeGitFlow() {
    if (!gitService_) return;
    gitService_->gitFlowInit();
}

void GitFlowWidget::onStartFeature() {
    if (!gitService_) return;
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Start Feature"),
                                         tr("Feature name:"),
                                         QLineEdit::Normal, {}, &ok);
    if (ok && !name.isEmpty()) {
        gitService_->featureStart(name);
    }
}

void GitFlowWidget::onFinishFeature() {
    if (!gitService_ || !featureList_->currentItem()) return;
    QString name = featureList_->currentItem()->text();
    gitService_->featureFinish(name);
}

void GitFlowWidget::onStartRelease() {
    if (!gitService_) return;
    bool ok = false;
    QString version = QInputDialog::getText(this, tr("Start Release"),
                                            tr("Release version:"),
                                            QLineEdit::Normal, {}, &ok);
    if (ok && !version.isEmpty()) {
        gitService_->releaseStart(version);
    }
}

void GitFlowWidget::onFinishRelease() {
    if (!gitService_ || !releaseList_->currentItem()) return;
    QString version = releaseList_->currentItem()->text();
    gitService_->releaseFinish(version);
}

void GitFlowWidget::onStartHotfix() {
    if (!gitService_) return;
    bool ok = false;
    QString version = QInputDialog::getText(this, tr("Start Hotfix"),
                                            tr("Hotfix version:"),
                                            QLineEdit::Normal, {}, &ok);
    if (ok && !version.isEmpty()) {
        gitService_->hotfixStart(version);
    }
}

void GitFlowWidget::onFinishHotfix() {
    if (!gitService_ || !hotfixList_->currentItem()) return;
    QString version = hotfixList_->currentItem()->text();
    gitService_->hotfixFinish(version);
}

void GitFlowWidget::onGitFlowOperationComplete(bool success, const QString& message) {
    if (success) {
        console_->appendOutput(message);
    } else {
        console_->appendError(message);
    }
    refresh();
}

} // namespace gitbolt::widgets
