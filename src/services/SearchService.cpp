#include "services/SearchService.h"

#include "git/GitProcess.h"
#include "git/Revwalk.h"

#include <QDateTime>
#include <algorithm>
#include <chrono>

namespace gitbolt::services {

SearchService::SearchService(QObject* parent)
    : QObject(parent)
    , runner_(this) {}

void SearchService::searchByMessage(git::Repository* repo, const QString& query) {
    if (!repo || searching_)
        return;

    searching_ = true;
    cancelRequested_ = false;
    emit searchStarted();

    std::string queryStr = query.toStdString();

    runner_.runWithResult<std::vector<git::CommitData>>(
        [repo, queryStr, this]() -> std::vector<git::CommitData> {
            std::vector<git::CommitData> results;

            auto walkResult = repo->createRevWalk();
            if (!walkResult)
                return results;

            auto& walk = *walkResult;
            walk.pushHead();
            walk.setSorting(git::SortOrder::Time);

            walk.walk([&](const git::CommitData& commit) -> bool {
                if (cancelRequested_)
                    return false;

                // Case-insensitive substring match on message
                auto pos = commit.message.find(queryStr);
                if (pos != std::string::npos) {
                    results.push_back(commit);
                } else {
                    // Also try case-insensitive
                    std::string msgLower = commit.message;
                    std::string qLower = queryStr;
                    std::transform(msgLower.begin(), msgLower.end(),
                                   msgLower.begin(), ::tolower);
                    std::transform(qLower.begin(), qLower.end(),
                                   qLower.begin(), ::tolower);
                    if (msgLower.find(qLower) != std::string::npos) {
                        results.push_back(commit);
                    }
                }

                return results.size() < 1000; // limit
            });

            return results;
        },
        [this](std::vector<git::CommitData> results) {
            emitResults(std::move(results));
        });
}

void SearchService::searchByAuthor(git::Repository* repo, const QString& query) {
    if (!repo || searching_)
        return;

    searching_ = true;
    cancelRequested_ = false;
    emit searchStarted();

    std::string queryStr = query.toStdString();

    runner_.runWithResult<std::vector<git::CommitData>>(
        [repo, queryStr, this]() -> std::vector<git::CommitData> {
            std::vector<git::CommitData> results;

            auto walkResult = repo->createRevWalk();
            if (!walkResult)
                return results;

            auto& walk = *walkResult;
            walk.pushHead();
            walk.setSorting(git::SortOrder::Time);

            walk.walk([&](const git::CommitData& commit) -> bool {
                if (cancelRequested_)
                    return false;

                // Match on author name or email (case-insensitive)
                std::string nameLower = commit.author.name;
                std::string emailLower = commit.author.email;
                std::string qLower = queryStr;

                std::transform(nameLower.begin(), nameLower.end(),
                               nameLower.begin(), ::tolower);
                std::transform(emailLower.begin(), emailLower.end(),
                               emailLower.begin(), ::tolower);
                std::transform(qLower.begin(), qLower.end(),
                               qLower.begin(), ::tolower);

                if (nameLower.find(qLower) != std::string::npos
                    || emailLower.find(qLower) != std::string::npos) {
                    results.push_back(commit);
                }

                return results.size() < 1000;
            });

            return results;
        },
        [this](std::vector<git::CommitData> results) {
            emitResults(std::move(results));
        });
}

void SearchService::searchByDateRange(git::Repository* repo,
                                       const QDate& from, const QDate& to) {
    if (!repo || searching_)
        return;

    searching_ = true;
    cancelRequested_ = false;
    emit searchStarted();

    // Convert QDate to time_point boundaries
    auto fromEpoch = QDateTime(from, QTime(0, 0, 0), Qt::LocalTime).toSecsSinceEpoch();
    auto toEpoch = QDateTime(to, QTime(23, 59, 59), Qt::LocalTime).toSecsSinceEpoch();

    auto fromTp = std::chrono::system_clock::from_time_t(
        static_cast<std::time_t>(fromEpoch));
    auto toTp = std::chrono::system_clock::from_time_t(
        static_cast<std::time_t>(toEpoch));

    runner_.runWithResult<std::vector<git::CommitData>>(
        [repo, fromTp, toTp, this]() -> std::vector<git::CommitData> {
            std::vector<git::CommitData> results;

            auto walkResult = repo->createRevWalk();
            if (!walkResult)
                return results;

            auto& walk = *walkResult;
            walk.pushHead();
            walk.setSorting(git::SortOrder::Time);

            walk.walk([&](const git::CommitData& commit) -> bool {
                if (cancelRequested_)
                    return false;

                auto commitTime = commit.author.when;
                if (commitTime >= fromTp && commitTime <= toTp) {
                    results.push_back(commit);
                }

                // If commit is before fromTp, we can stop (time-sorted)
                if (commitTime < fromTp)
                    return false;

                return results.size() < 5000;
            });

            return results;
        },
        [this](std::vector<git::CommitData> results) {
            emitResults(std::move(results));
        });
}

void SearchService::searchByFileContent(git::Repository* repo, const QString& query) {
    if (!repo || searching_)
        return;

    searching_ = true;
    cancelRequested_ = false;
    emit searchStarted();

    // Use git CLI: git log -S"text" --format="%H|%an|%ae|%at|%s"
    std::string workdir = repo->workdir();
    std::string queryStr = query.toStdString();

    runner_.runWithResult<std::vector<git::CommitData>>(
        [workdir, queryStr, this]() -> std::vector<git::CommitData> {
            std::vector<git::CommitData> results;

            git::GitProcess proc(workdir);
            std::string pickaxeArg = "-S" + queryStr;
            auto output = proc.run({
                "log",
                pickaxeArg,
                "--format=%H|%an|%ae|%at|%s",
                "-n", "500",
            });

            if (!output)
                return results;

            parseGitLogOutput(
                QString::fromStdString(output->stdoutData), results);
            return results;
        },
        [this](std::vector<git::CommitData> results) {
            emitResults(std::move(results));
        });
}

void SearchService::searchByFilePath(git::Repository* repo, const QString& path) {
    if (!repo || searching_)
        return;

    searching_ = true;
    cancelRequested_ = false;
    emit searchStarted();

    // Use git CLI: git log -- path --format="%H|%an|%ae|%at|%s"
    std::string workdir = repo->workdir();
    std::string pathStr = path.toStdString();

    runner_.runWithResult<std::vector<git::CommitData>>(
        [workdir, pathStr, this]() -> std::vector<git::CommitData> {
            std::vector<git::CommitData> results;

            git::GitProcess proc(workdir);
            auto output = proc.run({
                "log",
                "--format=%H|%an|%ae|%at|%s",
                "-n", "500",
                "--",
                pathStr,
            });

            if (!output)
                return results;

            parseGitLogOutput(
                QString::fromStdString(output->stdoutData), results);
            return results;
        },
        [this](std::vector<git::CommitData> results) {
            emitResults(std::move(results));
        });
}

void SearchService::cancel() {
    cancelRequested_ = true;
}

void SearchService::emitResults(std::vector<git::CommitData> results) {
    searching_ = false;
    cancelRequested_ = false;
    emit searchComplete(std::move(results));
}

void SearchService::parseGitLogOutput(const QString& output,
                                       std::vector<git::CommitData>& results) {
    const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const auto& line : lines) {
        if (cancelRequested_)
            break;

        // Format: %H|%an|%ae|%at|%s
        auto parts = line.split(QLatin1Char('|'));
        if (parts.size() < 5)
            continue;

        git::CommitData commit;
        commit.id = git::ObjectId::fromHex(parts[0].toStdString());

        commit.author.name = parts[1].toStdString();
        commit.author.email = parts[2].toStdString();

        bool ok = false;
        qint64 epoch = parts[3].toLongLong(&ok);
        if (ok) {
            commit.author.when = std::chrono::system_clock::from_time_t(
                static_cast<std::time_t>(epoch));
        }

        // Summary is everything from part[4] onwards (may contain '|')
        QStringList msgParts = parts.mid(4);
        commit.summary = msgParts.join(QLatin1Char('|')).toStdString();
        commit.message = commit.summary;

        results.push_back(std::move(commit));
    }
}

} // namespace gitbolt::services
