#include "dialogs/MaintenanceDialog.h"
#include "widgets/MaintenanceWidget.h"

#include <QDialogButtonBox>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

MaintenanceDialog::MaintenanceDialog(services::GitService* service,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Repository Maintenance"));
    setMinimumSize(500, 450);

    auto* layout = new QVBoxLayout(this);

    maintenanceWidget_ = new widgets::MaintenanceWidget(this);
    maintenanceWidget_->setGitService(service);
    maintenanceWidget_->refreshStats();
    layout->addWidget(maintenanceWidget_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);
}

} // namespace gitbolt::dialogs
