#pragma once

#include <QDialog>

namespace gitbolt::services {
class GitService;
}

namespace gitbolt::widgets {
class GitFlowWidget;
}

namespace gitbolt::dialogs {

class GitFlowDialog : public QDialog {
    Q_OBJECT
public:
    explicit GitFlowDialog(services::GitService* service,
                           QWidget* parent = nullptr);

    /// If git-flow is not initialized, show a setup wizard page first.
    void showSetupWizardIfNeeded();

private:
    void initializeWithDefaults();

    services::GitService* gitService_ = nullptr;
    widgets::GitFlowWidget* gitFlowWidget_ = nullptr;
};

} // namespace gitbolt::dialogs
