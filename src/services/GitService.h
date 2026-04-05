#pragma once
#include "git/Repository.h"
#include "util/AsyncRunner.h"
#include "watcher/FileWatcher.h"
#include <QObject>
#include <memory>

namespace gitbolt::services {

class GitService : public QObject {
    Q_OBJECT
public:
    explicit GitService(QObject* parent = nullptr);

    bool openRepository(const QString& path);
    void closeRepository();
    bool isOpen() const;
    git::Repository* repository() const;

    void refreshStatus();
    void refreshLog(int offset = 0, int count = 256);
    void refreshBranches();

    void stageFile(const QString& path);
    void unstageFile(const QString& path);
    void stageAll();
    void commitChanges(const QString& message, bool amend = false);

    void createBranch(const QString& name);
    void deleteBranch(const QString& name);
    void checkoutBranch(const QString& name);

    void push(const QString& remote, const QString& branch);
    void pull(const QString& remote, const QString& branch);
    void fetch(const QString& remote = "");

signals:
    void repositoryOpened(const QString& path);
    void repositoryClosed();
    void statusReady(std::vector<gitbolt::git::StatusEntry> entries);
    void logReady(std::vector<gitbolt::git::CommitData> commits, int offset);
    void branchesReady(std::vector<gitbolt::git::BranchInfo> branches);
    void commitComplete(bool success, const QString& message);
    void operationFailed(const QString& operation, const QString& error);
    void repositoryChanged();

private:
    std::unique_ptr<git::Repository> repo_;
    AsyncRunner runner_;
    FileWatcher watcher_;
};

} // namespace gitbolt::services
