#pragma once
#include <QObject>
#include <QFileSystemWatcher>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace gitbolt::watcher {

class FileWatcher : public QObject {
    Q_OBJECT
public:
    explicit FileWatcher(QObject* parent = nullptr);

    /// Synchronous, all-in-one watch setup. Equivalent to
    /// watchGitInternals() + addWatchPaths(enumerateWatchDirs()).
    /// Blocks on the working-tree walk — callers on the UI thread
    /// with potentially large repos should instead run
    /// enumerateWatchDirs() on a worker and feed the result to
    /// addWatchPaths() (see GitService::openRepositoryAsync).
    void watchRepository(const QString& repoPath);

    /// Cheap, immediate part of watch setup: watches .git itself
    /// (FETCH_HEAD / packed-refs creation), .git/index, .git/HEAD,
    /// packed-refs, FETCH_HEAD, the whole refs/ directory subtree
    /// (every dir — directoryChanged only fires for a watched dir's
    /// OWN listing, so refs/ alone never reports a write to
    /// refs/heads/main), and the repo root. External commits,
    /// fetches, branch creations and tag updates all land in one of
    /// these.
    void watchGitInternals(const QString& repoPath);

    /// Registers working-tree directories with the OS watcher.
    /// Must be called on this object's thread. No-ops if repoPath
    /// no longer matches the active repository (stale async result
    /// after a repo switch). Paths are applied in chunks across
    /// event-loop ticks so thousands of registrations don't stall
    /// the UI in one slice.
    void addWatchPaths(const QString& repoPath, const QStringList& dirs);

    /// Pure enumeration of the working-tree directories worth
    /// watching (skip list for .git, node_modules, build, … and a
    /// hard cap). No QFileSystemWatcher access — safe to run on any
    /// thread. `cancelled` is polled each iteration so an obsolete
    /// walk (repo switched, app quitting) can bail early.
    static QStringList enumerateWatchDirs(
        const QString& repoPath,
        int maxDirs = 4096,
        const std::function<bool()>& cancelled = {});

    void stop();

signals:
    void repositoryChanged();
    void indexChanged();
    void headChanged();

private slots:
    void onFileChanged(const QString& path);
    void onDirectoryChanged(const QString& path);

private:
    void applyNextChunk();
    /// Watch refs/ and every directory below it. Re-run whenever a
    /// directory under refs/ changes so newly created namespaces
    /// (first feature/* branch, a remote's first fetch) get their
    /// own watch before refs land inside them. Idempotent.
    void watchRefsSubtree();
    void addFileIfExists(const QString& path);

    QFileSystemWatcher watcher_;
    QTimer debounceTimer_;
    QString repoPath_;
    QString gitDir_;            // empty when no .git dir is watched
    QStringList pendingDirs_;   // queued for chunked addPaths
};

} // namespace gitbolt::watcher
