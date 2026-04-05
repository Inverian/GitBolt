#include "dialogs/GitFlowDialog.h"
#include "widgets/GitFlowWidget.h"
#include "services/GitService.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

GitFlowDialog::GitFlowDialog(services::GitService* service, QWidget* parent)
    : QDialog(parent), gitService_(service) {
    setWindowTitle(tr("Git Flow"));
    setMinimumSize(550, 500);

    auto* layout = new QVBoxLayout(this);

    gitFlowWidget_ = new widgets::GitFlowWidget(this);
    gitFlowWidget_->setGitService(service);
    gitFlowWidget_->refresh();
    layout->addWidget(gitFlowWidget_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void GitFlowDialog::showSetupWizardIfNeeded() {
    if (!gitService_ || !gitService_->isOpen()) return;
    if (gitService_->isGitFlowInitialized()) return;

    // Show a setup dialog with branch name configuration
    QDialog wizard(this);
    wizard.setWindowTitle(tr("Initialize Git Flow"));

    auto* layout = new QVBoxLayout(&wizard);

    auto* infoLabel = new QLabel(tr(
        "Git Flow is not initialized in this repository.\n"
        "Configure branch naming conventions below, or use defaults."));
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto* formLayout = new QFormLayout;

    auto* masterEdit = new QLineEdit(QStringLiteral("master"));
    auto* developEdit = new QLineEdit(QStringLiteral("develop"));
    auto* featurePrefixEdit = new QLineEdit(QStringLiteral("feature/"));
    auto* releasePrefixEdit = new QLineEdit(QStringLiteral("release/"));
    auto* hotfixPrefixEdit = new QLineEdit(QStringLiteral("hotfix/"));

    formLayout->addRow(tr("Production branch:"), masterEdit);
    formLayout->addRow(tr("Development branch:"), developEdit);
    formLayout->addRow(tr("Feature prefix:"), featurePrefixEdit);
    formLayout->addRow(tr("Release prefix:"), releasePrefixEdit);
    formLayout->addRow(tr("Hotfix prefix:"), hotfixPrefixEdit);

    layout->addLayout(formLayout);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &wizard);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Initialize"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &wizard, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &wizard, &QDialog::reject);

    if (wizard.exec() == QDialog::Accepted) {
        initializeWithDefaults();
    }
}

void GitFlowDialog::initializeWithDefaults() {
    if (!gitService_) return;
    gitService_->gitFlowInit();
    // After initialization completes, the widget will refresh via the signal
}

} // namespace gitbolt::dialogs
