#pragma once

#include <QDateTime>
#include <QString>

namespace gitbolt::services {

/// Lightweight snapshot of a repository's state, rendered on the
/// dashboard's Recent Repositories table. All fields are independently
/// optional-ish — if the repo couldn't be opened, `valid` is false and
/// every other field stays at its default. Callers should render a
/// neutral placeholder ("—" or similar) for any field that's empty /
/// zero / invalid.
struct RecentRepoInfo {
    QString   branch;         ///< e.g. "main"; empty for detached HEAD
    bool      detachedHead = false;
    int       dirtyCount = 0; ///< count of modified/staged/untracked files
    bool      hasUpstream = false;
    int       ahead  = 0;     ///< commits on local not yet in upstream
    int       behind = 0;     ///< commits on upstream not yet in local
    QDateTime lastCommit;     ///< committer time of HEAD; invalid if unknown
    bool      valid  = false; ///< false if Repository::open() failed
};

/// Open the repo at `path`, read everything the dashboard wants to
/// show, and close it. SAFE to call on a worker thread — each call
/// opens and destroys its own libgit2 repository handle, so there's
/// no shared mutable state between probes. Intentionally swallows
/// all errors into the `valid` / default-field pattern rather than
/// throwing; the dashboard renders whatever it can.
RecentRepoInfo probeRecentRepo(const QString& path);

} // namespace gitbolt::services
