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

// ---------------------------------------------------------------------------
// Interactive Rebase
// ---------------------------------------------------------------------------

void GitService::interactiveRebase(const git::RebasePlan& plan) {
    if (!repo_) return;

    // Build a GIT_SEQUENCE_EDITOR script from the plan.
    // Each line: <operation> <short-hash> <message>
    std::string script;
    for (const auto& op : plan.operations) {
        const char* verb = "pick";
        switch (op.type) {
        case git::RebaseOperationType::Pick:    verb = "pick";   break;
        case git::RebaseOperationType::Reword:  verb = "reword"; break;
        case git::RebaseOperationType::Edit:    verb = "edit";   break;
        case git::RebaseOperationType::Squash:  verb = "squash"; break;
        case git::RebaseOperationType::Fixup:   verb = "fixup";  break;
        case git::RebaseOperationType::Drop:    verb = "drop";   break;
        }
        script += verb;
        script += " ";
        script += op.commitId.toShortHex();
        script += " ";
        script += op.message;
        script += "\n";
    }

    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc),
                 onto = plan.onto.toHex(), script]() mutable {
        auto result = proc.interactiveRebase(onto, script);
        if (!result) {
            emit operationFailed(
                QStringLiteral("rebase"),
                QString::fromStdString(result.error().message()));
            emit rebaseComplete(false);
        } else {
            emit rebaseComplete(true);
        }
    });
}

void GitService::rebaseContinue() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.rebaseContinue();
        if (!result) {
            emit operationFailed(
                QStringLiteral("rebaseContinue"),
                QString::fromStdString(result.error().message()));
            emit rebaseComplete(false);
        } else {
            emit rebaseComplete(true);
            refreshStatus();
            refreshLog();
        }
    });
}

void GitService::rebaseAbort() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.rebaseAbort();
        if (!result) {
            emit operationFailed(
                QStringLiteral("rebaseAbort"),
                QString::fromStdString(result.error().message()));
        }
        refreshStatus();
        refreshLog();
    });
}

void GitService::rebaseSkip() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.rebaseSkip();
        if (!result) {
            emit operationFailed(
                QStringLiteral("rebaseSkip"),
                QString::fromStdString(result.error().message()));
            emit rebaseComplete(false);
        } else {
            emit rebaseComplete(true);
            refreshStatus();
            refreshLog();
        }
    });
}

// ---------------------------------------------------------------------------
// Cherry-pick
// ---------------------------------------------------------------------------

void GitService::cherryPick(const std::vector<git::ObjectId>& commits) {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r, commits]() {
        for (const auto& commitId : commits) {
            auto result = r->cherryPick(commitId);
            if (!result) {
                emit operationFailed(
                    QStringLiteral("cherryPick"),
                    QString::fromStdString(result.error().message()));
                emit cherryPickComplete(false, QString::fromStdString(result.error().message()));
                return;
            }
            if (result->hasConflicts) {
                emit cherryPickComplete(
                    false,
                    tr("Cherry-pick produced conflicts for commit %1")
                        .arg(QString::fromStdString(commitId.toShortHex())));
                refreshStatus();
                return;
            }
        }
        emit cherryPickComplete(true, tr("Cherry-pick completed successfully."));
        refreshStatus();
        refreshLog();
    });
}

// ---------------------------------------------------------------------------
// Stash
// ---------------------------------------------------------------------------

void GitService::stashSave(const QString& message, bool includeUntracked) {
    if (!repo_) return;
    auto result = repo_->stashSave(message.toStdString(), includeUntracked);
    if (!result) {
        emit operationFailed(
            QStringLiteral("stashSave"),
            QString::fromStdString(result.error().message()));
    } else {
        refreshStatus();
        refreshStashes();
    }
}

void GitService::stashApply(int index) {
    if (!repo_) return;
    auto result = repo_->stashApply(static_cast<size_t>(index));
    if (!result) {
        emit operationFailed(
            QStringLiteral("stashApply"),
            QString::fromStdString(result.error().message()));
    } else {
        refreshStatus();
    }
}

void GitService::stashPop(int index) {
    if (!repo_) return;
    auto result = repo_->stashPop(static_cast<size_t>(index));
    if (!result) {
        emit operationFailed(
            QStringLiteral("stashPop"),
            QString::fromStdString(result.error().message()));
    } else {
        refreshStatus();
        refreshStashes();
    }
}

void GitService::stashDrop(int index) {
    if (!repo_) return;
    auto result = repo_->stashDrop(static_cast<size_t>(index));
    if (!result) {
        emit operationFailed(
            QStringLiteral("stashDrop"),
            QString::fromStdString(result.error().message()));
    } else {
        refreshStashes();
    }
}

void GitService::refreshStashes() {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r]() {
        auto result = r->stashes();
        if (result) emit stashesReady(std::move(*result));
    });
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

void GitService::refreshTags() {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r]() {
        auto result = r->tags();
        if (result) emit tagsReady(std::move(*result));
    });
}

void GitService::createTag(const QString& name, const QString& target,
                            const QString& message, bool annotated) {
    if (!repo_) return;
    auto targetId = git::ObjectId::fromHex(target.toStdString());

    git::Result<void> result = [&]() -> git::Result<void> {
        if (annotated)
            return repo_->createTag(name.toStdString(), targetId, message.toStdString());
        else
            return repo_->createLightweightTag(name.toStdString(), targetId);
    }();

    if (!result)
        emit operationFailed("createTag", QString::fromStdString(result.error().message()));
    else
        refreshTags();
}

void GitService::deleteTag(const QString& name) {
    if (!repo_) return;
    auto result = repo_->deleteTag(name.toStdString());
    if (!result)
        emit operationFailed("deleteTag", QString::fromStdString(result.error().message()));
    else
        refreshTags();
}

// ---------------------------------------------------------------------------
// Submodules
// ---------------------------------------------------------------------------

void GitService::refreshSubmodules() {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r]() {
        auto result = r->submodules();
        if (result) emit submodulesReady(std::move(*result));
    });
}

void GitService::submoduleInit(const QString& name) {
    if (!repo_) return;
    auto proc = repo_->process();
    auto result = proc.run({"submodule", "init", name.toStdString()});
    if (!result)
        emit operationFailed("submoduleInit", QString::fromStdString(result.error().message()));
    else
        refreshSubmodules();
}

void GitService::submoduleUpdate(const QString& name) {
    if (!repo_) return;
    auto proc = repo_->process();
    auto result = proc.run({"submodule", "update", "--init", name.toStdString()});
    if (!result)
        emit operationFailed("submoduleUpdate", QString::fromStdString(result.error().message()));
    else
        refreshSubmodules();
}

// ---------------------------------------------------------------------------
// Worktrees
// ---------------------------------------------------------------------------

void GitService::refreshWorktrees() {
    if (!repo_) return;
    auto* r = repo_.get();
    runner_.run([this, r]() {
        auto result = r->worktrees();
        if (result) emit worktreesReady(std::move(*result));
    });
}

void GitService::addWorktree(const QString& name, const QString& path,
                              const QString& branch) {
    if (!repo_) return;
    auto result = repo_->addWorktree(name.toStdString(), path.toStdString(),
                                      branch.toStdString());
    if (!result)
        emit operationFailed("addWorktree", QString::fromStdString(result.error().message()));
    else
        refreshWorktrees();
}

void GitService::removeWorktree(const QString& name) {
    if (!repo_) return;
    auto result = repo_->removeWorktree(name.toStdString());
    if (!result)
        emit operationFailed("removeWorktree", QString::fromStdString(result.error().message()));
    else
        refreshWorktrees();
}

// ---------------------------------------------------------------------------
// Git Flow
// ---------------------------------------------------------------------------

bool GitService::isGitFlowInitialized() {
    if (!repo_) return false;
    auto cfg = repo_->config();
    auto master = cfg.getString("gitflow.branch.master");
    return master.ok();
}

void GitService::gitFlowInit() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.gitFlowInit();
        if (!result) {
            emit operationFailed("gitFlowInit",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true, tr("Git Flow initialized."));
        }
    });
}

void GitService::featureStart(const QString& name) {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc), n = name.toStdString()]() mutable {
        auto result = proc.gitFlowFeatureStart(n);
        if (!result) {
            emit operationFailed("featureStart",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Feature '%1' started.").arg(QString::fromStdString(n)));
            refreshBranches();
        }
    });
}

void GitService::featureFinish(const QString& name) {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc), n = name.toStdString()]() mutable {
        auto result = proc.gitFlowFeatureFinish(n);
        if (!result) {
            emit operationFailed("featureFinish",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Feature '%1' finished.").arg(QString::fromStdString(n)));
            refreshBranches();
            refreshLog();
        }
    });
}

void GitService::releaseStart(const QString& version) {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowReleaseStart(v);
        if (!result) {
            emit operationFailed("releaseStart",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Release '%1' started.").arg(QString::fromStdString(v)));
            refreshBranches();
        }
    });
}

void GitService::releaseFinish(const QString& version) {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowReleaseFinish(v);
        if (!result) {
            emit operationFailed("releaseFinish",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Release '%1' finished.").arg(QString::fromStdString(v)));
            refreshBranches();
            refreshLog();
            refreshTags();
        }
    });
}

void GitService::hotfixStart(const QString& version) {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowHotfixStart(v);
        if (!result) {
            emit operationFailed("hotfixStart",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Hotfix '%1' started.").arg(QString::fromStdString(v)));
            refreshBranches();
        }
    });
}

void GitService::hotfixFinish(const QString& version) {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowHotfixFinish(v);
        if (!result) {
            emit operationFailed("hotfixFinish",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Hotfix '%1' finished.").arg(QString::fromStdString(v)));
            refreshBranches();
            refreshLog();
            refreshTags();
        }
    });
}

QStringList GitService::activeFeatures() {
    if (!repo_) return {};
    auto proc = repo_->process();
    auto result = proc.run({"branch", "--list", "feature/*"});
    if (!result) return {};
    QStringList names;
    const auto lines = QString::fromStdString(result->stdoutData).split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        QString trimmed = line.trimmed().remove(0, line.indexOf("feature/") >= 0 ? 0 : 0);
        // Strip leading "* " for current branch indicator
        if (trimmed.startsWith("* ")) trimmed = trimmed.mid(2);
        // Extract just the name after "feature/"
        int idx = trimmed.indexOf("feature/");
        if (idx >= 0)
            names.append(trimmed.mid(idx + 8));
    }
    return names;
}

QStringList GitService::activeReleases() {
    if (!repo_) return {};
    auto proc = repo_->process();
    auto result = proc.run({"branch", "--list", "release/*"});
    if (!result) return {};
    QStringList names;
    const auto lines = QString::fromStdString(result->stdoutData).split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("* ")) trimmed = trimmed.mid(2);
        int idx = trimmed.indexOf("release/");
        if (idx >= 0)
            names.append(trimmed.mid(idx + 8));
    }
    return names;
}

QStringList GitService::activeHotfixes() {
    if (!repo_) return {};
    auto proc = repo_->process();
    auto result = proc.run({"branch", "--list", "hotfix/*"});
    if (!result) return {};
    QStringList names;
    const auto lines = QString::fromStdString(result->stdoutData).split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("* ")) trimmed = trimmed.mid(2);
        int idx = trimmed.indexOf("hotfix/");
        if (idx >= 0)
            names.append(trimmed.mid(idx + 7));
    }
    return names;
}

// ---------------------------------------------------------------------------
// Repository Maintenance
// ---------------------------------------------------------------------------

void GitService::runGc() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.gc();
        if (!result) {
            emit operationFailed("gc",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

void GitService::runPrune() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.prune();
        if (!result) {
            emit operationFailed("prune",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

void GitService::runFsck() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.fsck();
        if (!result) {
            emit operationFailed("fsck",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

void GitService::runRepack() {
    if (!repo_) return;
    auto proc = repo_->process();
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.run({"repack", "-a", "-d"});
        if (!result) {
            emit operationFailed("repack",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

QString GitService::repositoryDiskUsage() {
    if (!repo_) return {};
    auto proc = repo_->process();
    auto result = proc.run({"count-objects", "-vH"});
    if (!result) return tr("Unable to determine disk usage.");
    return QString::fromStdString(result->stdoutData);
}

} // namespace gitbolt::services
