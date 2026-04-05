#pragma once

#include <QDialog>

namespace gitbolt::services {
class GitService;
}

namespace gitbolt::widgets {
class MaintenanceWidget;
}

namespace gitbolt::dialogs {

class MaintenanceDialog : public QDialog {
    Q_OBJECT
public:
    explicit MaintenanceDialog(services::GitService* service,
                               QWidget* parent = nullptr);

private:
    widgets::MaintenanceWidget* maintenanceWidget_ = nullptr;
};

} // namespace gitbolt::dialogs
