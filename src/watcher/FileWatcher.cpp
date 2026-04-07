#include "watcher/FileWatcher.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace gitbolt::watcher {

FileWatcher::FileWatcher(QObject* parent) : QObject(parent) {
    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(200);
    connect(&debounceTimer_, &QTimer::timeout, this, &FileWatcher::repositoryChanged);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &FileWatcher::onFileChanged);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &FileWatcher::onDirectoryChanged);
}

void FileWatcher::watchRepository(const QString& repoPath) {
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

    // Also watch the working tree so editing a file in an external
    // editor triggers a status refresh in GitBolt. QFileSystemWatcher
    // on macOS uses FSEvents under the hood, which is efficient for
    // directory-level monitoring even on large trees.
    //
    // Implementation note: we walk the tree once at open time and
    // add every directory (skipping .git/ and anything matching a
    // few noisy patterns like node_modules/, build/, and .venv/ that
    // are almost always in .gitignore). We cap the total number of
    // watched paths at 4096 to keep kqueue/FSEvents happy on very
    // large repos — if a repo exceeds that, the user won't get the
    // auto-refresh for the unwatched subtrees but everything else
    // keeps working.
    QDirIterator it(repoPath,
                    QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);

    int watchedDirs = 0;
    const int maxDirs = 4096;

    while (it.hasNext() && watchedDirs < maxDirs) {
        const QString dir = it.next();
        const QFileInfo fi(dir);
        const QString name = fi.fileName();

        // Skip .git (we already watch it selectively above) and
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

        watcher_.addPath(dir);
        ++watchedDirs;
    }
    // Also watch the repo root itself.
    watcher_.addPath(repoPath);
}

void FileWatcher::stop() {
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
