#include "services/GitService.h"

namespace gitbolt::services {

GitService::GitService(QObject* parent)
    : QObject(parent), runner_(this), watcher_(this) {
    connect(&watcher_, &FileWatcher::repositoryChanged, this, [this]() {
        refreshStatus();
        emit repositoryChanged();
    });
}

bool GitService::openRepository(const QString& path) {
    auto result = core::Repository::open(path.toStdString());
    if (!result) return false;
    repo_ = std::make_unique<core::Repository>(std::move(*result));
    watcher_.watchRepository(path);
    emit repositoryOpened(path);
    refreshStatus();
    refreshLog();
    refreshBranches();
    return true;
}

void GitService::closeRepository() {
    watcher_.stop();
    repo_.reset();
    emit repositoryClosed();
}

bool GitService::isOpen() const { return repo_ != nullptr; }
core::Repository* GitService::repository() const { return repo_.get(); }

void GitService::refreshStatus() {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r]() {
        auto result = r->status();
        if (result) emit statusReady(std::move(*result));
    });
}

void GitService::refreshLog(int offset, int count) {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r, offset, count]() {
        auto walk = r->createRevWalk();
        if (!walk) return;
        walk->setSorting(core::SortOrder::TopologicalTime);
        walk->pushHead();
        auto commits = walk->next(static_cast<size_t>(count));
        if (commits) emit logReady(std::move(*commits), offset);
    });
}

void GitService::refreshBranches() {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r]() {
        auto result = r->allBranches();
        if (result) emit branchesReady(std::move(*result));
    });
}

void GitService::stageFile(const QString& path) {
    if (!repo_) return;
    repo_->stageFile(path.toStdString());
    refreshStatus();
}

void GitService::unstageFile(const QString& path) {
    if (!repo_) return;
    repo_->unstageFile(path.toStdString());
    refreshStatus();
}

void GitService::stageAll() {
    if (!repo_) return;
    repo_->stageAll();
    refreshStatus();
}

void GitService::commitChanges(const QString& message, bool amend) {
    if (!repo_) return;
    auto result = repo_->commit(message.toStdString(), amend);
    if (result) {
        emit commitComplete(true, "Commit created: " + QString::fromStdString(result->toShortHex()));
        refreshStatus();
        refreshLog();
    } else {
        emit commitComplete(false, QString::fromStdString(result.error().message()));
    }
}

void GitService::createBranch(const QString& name) {
    if (!repo_) return;
    auto headResult = repo_->head();
    if (!headResult) return;
    repo_->createBranch(name.toStdString(), *headResult);
    refreshBranches();
}

void GitService::deleteBranch(const QString& name) {
    if (!repo_) return;
    repo_->deleteBranch(name.toStdString());
    refreshBranches();
}

void GitService::checkoutBranch(const QString& name) {
    if (!repo_) return;
    repo_->checkout(name.toStdString());
    refreshStatus();
    refreshLog();
    refreshBranches();
}

void GitService::push(const QString& remote, const QString& branch) {
    if (!repo_) return;
    auto proc = repo_->process();
    auto result = proc.push(remote.toStdString(), branch.toStdString());
    if (!result) emit operationFailed("push", QString::fromStdString(result.error().message()));
}

void GitService::pull(const QString& remote, const QString& branch) {
    if (!repo_) return;
    auto proc = repo_->process();
    auto result = proc.pull(remote.toStdString(), branch.toStdString());
    if (!result) emit operationFailed("pull", QString::fromStdString(result.error().message()));
    else { refreshStatus(); refreshLog(); refreshBranches(); }
}

void GitService::fetch(const QString& remote) {
    if (!repo_) return;
    auto proc = repo_->process();
    auto result = proc.fetch(remote.toStdString());
    if (!result) emit operationFailed("fetch", QString::fromStdString(result.error().message()));
    else refreshBranches();
}

} // namespace gitbolt::services
