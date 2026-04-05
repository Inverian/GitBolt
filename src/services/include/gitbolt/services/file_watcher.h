#pragma once
#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>

namespace gitbolt::services {

class FileWatcher : public QObject {
    Q_OBJECT
public:
    explicit FileWatcher(QObject* parent = nullptr);
    void watchRepository(const QString& repoPath);
    void stop();

signals:
    void repositoryChanged();
    void indexChanged();
    void headChanged();

private slots:
    void onFileChanged(const QString& path);
    void onDirectoryChanged(const QString& path);

private:
    QFileSystemWatcher watcher_;
    QTimer debounceTimer_;
    QString repoPath_;
};

} // namespace gitbolt::services
