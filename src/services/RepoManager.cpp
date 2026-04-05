#include "services/RepoManager.h"

#include <QDir>
#include <algorithm>

namespace gitbolt::services {

RepoManager::RepoManager(QObject* parent)
    : QObject(parent) {}

RepoManager::~RepoManager() {
    closeAll();
}

GitService* RepoManager::openRepository(const QString& path) {
    QString normalized = normalizePath(path);

    // Check if already open
    for (auto& entry : repos_) {
        if (entry.normalizedPath == normalized)
            return entry.service.get();
    }

    // Create new GitService and open the repository
    auto service = std::make_unique<GitService>(this);
    if (!service->openRepository(normalized)) {
        return nullptr;
    }

    GitService* raw = service.get();
    repos_.push_back({normalized, std::move(service)});

    // If this is the first repo, make it active
    if (repos_.size() == 1) {
        activePath_ = normalized;
        emit activeRepositoryChanged(normalized, raw);
    }

    emit repositoryOpened(normalized, raw);
    return raw;
}

void RepoManager::closeRepository(const QString& path) {
    QString normalized = normalizePath(path);

    auto it = std::find_if(repos_.begin(), repos_.end(),
                           [&](const RepoEntry& e) {
                               return e.normalizedPath == normalized;
                           });

    if (it == repos_.end())
        return;

    it->service->closeRepository();
    repos_.erase(it);

    emit repositoryClosed(normalized);

    // If the closed repo was active, switch to the first available
    if (activePath_ == normalized) {
        if (!repos_.empty()) {
            activePath_ = repos_.front().normalizedPath;
            emit activeRepositoryChanged(activePath_,
                                          repos_.front().service.get());
        } else {
            activePath_.clear();
            emit activeRepositoryChanged(QString(), nullptr);
        }
    }
}

void RepoManager::closeAll() {
    while (!repos_.empty()) {
        auto& entry = repos_.back();
        QString path = entry.normalizedPath;
        entry.service->closeRepository();
        repos_.pop_back();
        emit repositoryClosed(path);
    }
    activePath_.clear();
    emit activeRepositoryChanged(QString(), nullptr);
}

GitService* RepoManager::activeRepository() const {
    if (activePath_.isEmpty())
        return nullptr;

    for (auto& entry : repos_) {
        if (entry.normalizedPath == activePath_)
            return entry.service.get();
    }

    return nullptr;
}

void RepoManager::setActiveRepository(const QString& path) {
    QString normalized = normalizePath(path);

    if (normalized == activePath_)
        return;

    for (auto& entry : repos_) {
        if (entry.normalizedPath == normalized) {
            activePath_ = normalized;
            emit activeRepositoryChanged(normalized, entry.service.get());
            return;
        }
    }
}

bool RepoManager::isOpen(const QString& path) const {
    QString normalized = normalizePath(path);
    return std::any_of(repos_.begin(), repos_.end(),
                       [&](const RepoEntry& e) {
                           return e.normalizedPath == normalized;
                       });
}

QStringList RepoManager::openPaths() const {
    QStringList paths;
    paths.reserve(static_cast<int>(repos_.size()));
    for (auto& entry : repos_) {
        paths.append(entry.normalizedPath);
    }
    return paths;
}

int RepoManager::openCount() const {
    return static_cast<int>(repos_.size());
}

GitService* RepoManager::repositoryAt(const QString& path) const {
    QString normalized = normalizePath(path);
    for (auto& entry : repos_) {
        if (entry.normalizedPath == normalized)
            return entry.service.get();
    }
    return nullptr;
}

QString RepoManager::normalizePath(const QString& path) const {
    return QDir::cleanPath(QDir(path).absolutePath());
}

} // namespace gitbolt::services
