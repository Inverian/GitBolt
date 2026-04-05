#pragma once

#include "services/GitService.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

namespace gitbolt::services {

class RepoManager : public QObject {
    Q_OBJECT
public:
    explicit RepoManager(QObject* parent = nullptr);
    ~RepoManager() override;

    // Open a repository at the given path. Returns the GitService* for it.
    // If already open, returns the existing instance.
    GitService* openRepository(const QString& path);

    // Close and remove a repository by path.
    void closeRepository(const QString& path);

    // Close all repositories.
    void closeAll();

    // Access the currently active (focused) repository.
    GitService* activeRepository() const;

    // Set which open repository is the active one.
    void setActiveRepository(const QString& path);

    // Query
    bool isOpen(const QString& path) const;
    QStringList openPaths() const;
    int openCount() const;

    // Iterate all open repos
    GitService* repositoryAt(const QString& path) const;

signals:
    void repositoryOpened(const QString& path, GitService* service);
    void repositoryClosed(const QString& path);
    void activeRepositoryChanged(const QString& path, GitService* service);

private:
    struct RepoEntry {
        QString normalizedPath;
        std::unique_ptr<GitService> service;
    };

    QString normalizePath(const QString& path) const;

    std::vector<RepoEntry> repos_;
    QString activePath_;
};

} // namespace gitbolt::services
