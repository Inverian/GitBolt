#include "watcher/FileWatcher.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>

namespace gitbolt::watcher {

namespace {
// How many directories to register with QFileSystemWatcher per
// event-loop tick. Each addPaths() call restarts the underlying
// FSEvents stream on macOS (and re-registers inotify watches on
// Linux), so we want few, large batches — but a single 4096-path
// batch can still take long enough to stutter the UI right when
// the repo view is animating in. 512 per tick keeps each slice
// well under a frame while finishing the whole cap in 8 ticks.
constexpr int kAddPathsChunk = 512;
} // namespace

FileWatcher::FileWatcher(QObject* parent) : QObject(parent) {
    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(200);
    connect(&debounceTimer_, &QTimer::timeout, this, &FileWatcher::repositoryChanged);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &FileWatcher::onFileChanged);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &FileWatcher::onDirectoryChanged);
}

void FileWatcher::watchRepository(const QString& repoPath) {
    watchGitInternals(repoPath);
    addWatchPaths(repoPath, enumerateWatchDirs(repoPath));
}

void FileWatcher::watchGitInternals(const QString& repoPath) {
    stop();
    repoPath_ = repoPath;

    // Watch .git/ internals so we notice commits, checkouts, and index
    // updates made by external tools (or by our own GitService after
    // a stage/commit cycle).
    QString gitDir = repoPath + "/.git";
    if (QDir(gitDir).exists()) {
        watcher_.addPath(gitDir + "/index");
        watcher_.addPath(gitDir + "/HEAD");
        watcher_.addPath(gitDir + "/refs");
    }

    // Watch the repo root immediately too — root-level edits are
    // noticed even while the full working-tree walk is still
    // running on a worker thread.
    watcher_.addPath(repoPath);
}

QStringList FileWatcher::enumerateWatchDirs(
    const QString& repoPath,
    int maxDirs,
    const std::function<bool()>& cancelled)
{
    // Walk the working tree and collect every directory (skipping
    // .git/ and a few noisy patterns like node_modules/, build/,
    // and .venv/ that are almost always in .gitignore). The total
    // is capped to keep kqueue/FSEvents happy on very large repos —
    // if a repo exceeds the cap, the user won't get auto-refresh
    // for the unwatched subtrees but everything else keeps working.
    //
    // Pure filesystem work, no QFileSystemWatcher access: safe to
    // run on a worker thread, which is exactly what GitService does
    // for the async open path (this walk stats every directory and
    // took ~a minute on large repos when it ran on the UI thread).
    QStringList dirs;
    QDirIterator it(repoPath,
                    QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);

    while (it.hasNext() && dirs.size() < maxDirs) {
        if (cancelled && cancelled())
            return {};

        const QString dir = it.next();
        const QFileInfo fi(dir);
        const QString name = fi.fileName();

        // Skip .git (watched selectively by watchGitInternals) and
        // common high-churn directories that are almost always
        // ignored by git anyway.
        if (name == QStringLiteral(".git")
            || name == QStringLiteral("node_modules")
            || name == QStringLiteral("build")
            || name == QStringLiteral("build-debug")
            || name == QStringLiteral("build-release")
            || name == QStringLiteral(".venv")
            || name == QStringLiteral("venv")
            || name == QStringLiteral(".tox")
            || name == QStringLiteral("__pycache__")
            || name == QStringLiteral(".idea")
            || name == QStringLiteral(".vscode")
            || name == QStringLiteral("target")      // Rust
            || name == QStringLiteral(".next")       // Next.js
            || name == QStringLiteral(".cache")) {
            // Skip this subtree entirely.
            continue;
        }

        dirs.append(dir);
    }
    return dirs;
}

void FileWatcher::addWatchPaths(const QString& repoPath, const QStringList& dirs)
{
    // Stale async result — the user already switched to another
    // repo (or closed this one) while the enumeration was running.
    if (repoPath != repoPath_ || dirs.isEmpty())
        return;

    pendingDirs_ = dirs;
    applyNextChunk();
}

void FileWatcher::applyNextChunk()
{
    // repoPath_ cleared (stop()/repo switch) invalidates the queue.
    if (pendingDirs_.isEmpty())
        return;

    const int n = std::min<int>(kAddPathsChunk, pendingDirs_.size());
    watcher_.addPaths(pendingDirs_.mid(0, n));
    pendingDirs_ = pendingDirs_.mid(n);

    if (!pendingDirs_.isEmpty()) {
        // Yield back to the event loop between chunks so a long
        // tail of registrations never blocks input or painting.
        QMetaObject::invokeMethod(this, &FileWatcher::applyNextChunk,
                                  Qt::QueuedConnection);
    }
}

void FileWatcher::stop() {
    pendingDirs_.clear();
    repoPath_.clear();
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
}

void FileWatcher::onFileChanged(const QString& path) {
    if (path.endsWith("/index")) emit indexChanged();
    else if (path.endsWith("/HEAD")) emit headChanged();
    debounceTimer_.start();
}

void FileWatcher::onDirectoryChanged(const QString& /*path*/) {
    debounceTimer_.start();
}

} // namespace gitbolt::watcher
