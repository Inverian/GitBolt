#pragma once

#include "git/Commit.h"
#include "git/Repository.h"
#include "util/AsyncRunner.h"

#include <QDate>
#include <QObject>
#include <vector>

namespace gitbolt::services {

class SearchService : public QObject {
    Q_OBJECT
public:
    explicit SearchService(QObject* parent = nullptr);

    // All search methods run on background threads via AsyncRunner.
    // Results are delivered through the searchComplete signal.

    void searchByMessage(gitbolt::git::Repository* repo, const QString& query);
    void searchByAuthor(gitbolt::git::Repository* repo, const QString& query);
    void searchByDateRange(gitbolt::git::Repository* repo,
                           const QDate& from, const QDate& to);
    void searchByFileContent(gitbolt::git::Repository* repo, const QString& query);
    void searchByFilePath(gitbolt::git::Repository* repo, const QString& path);

    void cancel();
    bool isSearching() const { return searching_; }

signals:
    void searchComplete(std::vector<gitbolt::git::CommitData> results);
    void searchFailed(const QString& error);
    void searchStarted();

private:
    void emitResults(std::vector<gitbolt::git::CommitData> results);
    void parseGitLogOutput(const QString& output,
                           std::vector<gitbolt::git::CommitData>& results);

    gitbolt::util::AsyncRunner runner_;
    std::atomic<bool> searching_{false};
    std::atomic<bool> cancelRequested_{false};
};

} // namespace gitbolt::services
