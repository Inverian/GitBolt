#include "ui/RepoCommandController.h"

#include "services/GitService.h"

#include <QFutureWatcher>
#include <QMessageBox>
#include <QPointer>
#include <QtConcurrent>

#include <memory>
#include <optional>

namespace gitbolt::ui {

RepoCommandController::RepoCommandController(services::GitService* svc,
                                             QWidget* dialogParent,
                                             QObject* parent)
    : QObject(parent), svc_(svc), dialogParent_(dialogParent)
{
}

bool RepoCommandController::runJob(const QString& busyMessage,
                                   std::function<void(const git::GitProcess&)> work,
                                   std::function<void()> finished,
                                   RefreshSet refresh)
{
    if (!svc_ || !svc_->isOpen())
        return false;
    // One at a time: two of them would be two gits at one index (a
    // reset under a merge), and the window can only show one.
    if (busy_) {
        emit refused();
        return false;
    }

    // Snapshotted here, on the GUI thread; the job touches nothing of
    // the service's. The generation tells whether the repository the
    // op ran on is still the open one when it ends.
    const git::GitProcess proc = svc_->process();
    const quint64 generation = svc_->openGeneration();
    busy_ = true;
    emit busyChanged(true, busyMessage);

    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this,
            [this, watcher, generation, refresh, finished = std::move(finished)]() {
        watcher->deleteLater();
        busy_ = false;
        emit busyChanged(false, QString());
        if (finished && generation == svc_->openGeneration())
            finished();
        refreshAfter(refresh);
    });
    job_ = QtConcurrent::run([proc, work = std::move(work)]() { work(proc); });
    watcher->setFuture(job_);
    return true;
}

bool RepoCommandController::runAsync(const std::vector<std::string>& args,
                                     const QString& failTitle,
                                     RefreshSet refresh,
                                     const QString& busyMessage,
                                     std::function<void(bool)> done,
                                     QWidget* parentOverride,
                                     int timeoutMs)
{
    // Result has no empty state, so the job leaves it in an optional.
    auto out = std::make_shared<std::optional<git::Result<git::ProcessOutput>>>();
    // A modeless dialog (Reflog) may be closed by the time git is done.
    QPointer<QWidget> parentWidget = parentOverride;
    return runJob(
        busyMessage,
        [out, args, timeoutMs](const git::GitProcess& proc) {
            out->emplace(proc.run(args, timeoutMs));
        },
        [this, out, failTitle, parentWidget, done = std::move(done)]() {
            if (!out->has_value())
                return;
            const git::Result<git::ProcessOutput>& result = **out;
            QWidget* parent = parentWidget ? parentWidget.data() : dialogParent_;
            bool ok = false;
            if (!result.ok()) {
                QMessageBox::warning(parent, failTitle,
                    QString::fromStdString(result.error().message()));
            } else if (!result.value().success()) {
                QString detail =
                    QString::fromStdString(result.value().stderrData).trimmed();
                if (detail.isEmpty())
                    detail =
                        QString::fromStdString(result.value().stdoutData).trimmed();
                if (detail.isEmpty())
                    detail = tr("git exited with code %1")
                                 .arg(result.value().exitCode);
                QMessageBox::warning(parent, failTitle, detail);
            } else {
                ok = true;
            }
            if (done)
                done(ok);
        },
        refresh);
}

void RepoCommandController::waitForFinished()
{
    job_.waitForFinished();
}

void RepoCommandController::refreshAfter(RefreshSet refresh)
{
    if (refresh & RefreshStatus)     svc_->refreshStatus();
    if (refresh & RefreshLog)        svc_->refreshLog();
    if (refresh & RefreshBranches)   svc_->refreshBranches();
    if (refresh & RefreshStashes)    svc_->refreshStashes();
    if (refresh & RefreshSubmodules) svc_->refreshSubmodules();
    if (refresh & RefreshTags)       svc_->refreshTags();
}

} // namespace gitbolt::ui
