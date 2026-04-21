#include "services/RecentRepoProbe.h"

#include "git/Repository.h"

#include <QTimeZone>
#include <chrono>

namespace gitbolt::services {

RecentRepoInfo probeRecentRepo(const QString& path)
{
    RecentRepoInfo out;

    // Open the repo. Anything other than success (missing path, not a
    // git repo, permission denied, etc.) falls through with valid=false
    // and the dashboard renders placeholders for every field.
    auto repoResult = git::Repository::open(path.toStdString());
    if (!repoResult.ok())
        return out;
    auto& repo = repoResult.value();

    out.valid = true;

    // --- Branch name --------------------------------------------------
    //
    // headBranchName() returns the short name (e.g. "main"). If HEAD is
    // detached, it'll fail / be empty — we mark detachedHead in that case
    // so the UI can show "(detached)" instead of a blank cell.
    if (repo.isHeadDetached()) {
        out.detachedHead = true;
    } else {
        auto nameRes = repo.headBranchName();
        if (nameRes.ok())
            out.branch = QString::fromStdString(nameRes.value());
    }

    // --- Dirty file count --------------------------------------------
    //
    // Iterate the status entries and count anything staged, in the
    // working tree, or untracked. Conflicts count too — they're a
    // strong signal the repo has pending work. Ignored files are not
    // counted (that's the expected behavior; ignored noise isn't
    // "dirty" to the user).
    {
        auto statusRes = repo.status();
        if (statusRes.ok()) {
            for (const auto& entry : statusRes.value()) {
                if (entry.isStaged() || entry.isWorkingTree() ||
                    entry.isUntracked() || entry.isConflicted())
                    ++out.dirtyCount;
            }
        }
    }

    // --- Ahead / behind ----------------------------------------------
    //
    // branches(Local) returns every local branch; we want the one
    // currently at HEAD (isHead == true). If that branch has an
    // upstream configured, aheadCount / behindCount are populated.
    // Absent upstream → hasUpstream stays false, dashboard renders "—".
    {
        auto branchesRes = repo.branches(git::BranchType::Local);
        if (branchesRes.ok()) {
            for (const auto& b : branchesRes.value()) {
                if (!b.isHead)
                    continue;
                if (!b.upstream.empty()) {
                    out.hasUpstream = true;
                    out.ahead  = b.aheadCount;
                    out.behind = b.behindCount;
                }
                break;
            }
        }
    }

    // --- Last commit date --------------------------------------------
    //
    // HEAD → commit lookup → committer time_point. Using the committer
    // time (rather than author time) matches what `git log -1` shows by
    // default and what most GUIs display — it's the time the commit
    // actually landed on this branch, not when it was originally
    // authored (which can differ across rebase/cherry-pick).
    {
        auto headRes = repo.head();
        if (headRes.ok()) {
            auto commitRes = repo.lookupCommit(headRes.value());
            if (commitRes.ok()) {
                // system_clock::time_point → QDateTime via epoch-seconds
                // round-trip. Qt's QDateTime::fromSecsSinceEpoch takes a
                // qint64, which std::chrono's duration_cast gives us.
                const auto tp = commitRes.value().committer.when;
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    tp.time_since_epoch()).count();
                out.lastCommit = QDateTime::fromSecsSinceEpoch(
                    static_cast<qint64>(secs), QTimeZone::UTC);
            }
        }
    }

    return out;
}

} // namespace gitbolt::services
