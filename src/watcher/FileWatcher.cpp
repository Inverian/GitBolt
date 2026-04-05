#include "watcher/FileWatcher.h"
#include <QDir>

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
    QString gitDir = repoPath + "/.git";
    if (QDir(gitDir).exists()) {
        watcher_.addPath(gitDir + "/index");
        watcher_.addPath(gitDir + "/HEAD");
        watcher_.addPath(gitDir + "/refs");
    }
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
