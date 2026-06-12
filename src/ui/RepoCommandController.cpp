#include "ui/RepoCommandController.h"

#include "services/GitService.h"

#include <QMessageBox>

namespace gitbolt::ui {

RepoCommandController::RepoCommandController(services::GitService* svc,
                                             QWidget* dialogParent,
                                             QObject* parent)
    : QObject(parent), svc_(svc), dialogParent_(dialogParent)
{
}

bool RepoCommandController::run(const std::vector<std::string>& args,
                                const QString& failTitle,
                                RefreshSet refresh,
                                QWidget* parentOverride,
                                int timeoutMs)
{
    if (!svc_ || !svc_->isOpen())
        return false;

    const auto out = svc_->process().run(args, timeoutMs);

    bool ok = false;
    QWidget* parent = parentOverride ? parentOverride : dialogParent_;
    if (!out.ok()) {
        QMessageBox::warning(parent, failTitle,
            QString::fromStdString(out.error().message()));
    } else if (!out.value().success()) {
        QString detail =
            QString::fromStdString(out.value().stderrData).trimmed();
        if (detail.isEmpty())
            detail =
                QString::fromStdString(out.value().stdoutData).trimmed();
        if (detail.isEmpty())
            detail = tr("git exited with code %1")
                         .arg(out.value().exitCode);
        QMessageBox::warning(parent, failTitle, detail);
    } else {
        ok = true;
    }

    if (refresh & RefreshStatus)     svc_->refreshStatus();
    if (refresh & RefreshLog)        svc_->refreshLog();
    if (refresh & RefreshBranches)   svc_->refreshBranches();
    if (refresh & RefreshStashes)    svc_->refreshStashes();
    if (refresh & RefreshSubmodules) svc_->refreshSubmodules();
    if (refresh & RefreshTags)       svc_->refreshTags();

    return ok;
}

} // namespace gitbolt::ui
