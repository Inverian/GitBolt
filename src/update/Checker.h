#pragma once

#include "update/Release.h"
#include "update/UpdateConfig.h"
#include "update/Version.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace gitbolt::update {

/// Asks the feed (Config::feedUrl, GitHub's releases/latest) whether a
/// newer release is out. One request at a time, on the GUI thread.
///
/// The network stack is created at the first check(), not before, so
/// loading the TLS backend never lands on the launch path (as in
/// telemetry), and nothing runs on the global thread pool, which
/// main() waits for at exit.
///
/// The request: GET, with Accept: application/vnd.github+json,
/// X-GitHub-Api-Version: 2022-11-28 and the policy of Request.h
/// (User-Agent, no cookies, no cache, the transfer timeout). It
/// follows no redirect: the feed is one fixed URL, and a 3xx is an
/// answer like any other non-200. An answer over 4 MiB is cut off
/// (a real one is a few kilobytes).
///
/// Every outcome comes as one signal, always from the event loop,
/// never from inside check(), and with busy() already false, so a
/// slot may check again:
///   - available: the release parsed (parseRelease) and is newer than
///     Config::current;
///   - upToDate: it parsed and is not newer, including when it is
///     older than the running copy (a development build);
///   - failed: a sentence for the dialog. GitHub's rate limit (403
///     with x-ratelimit-remaining: 0 or retry-after, or any 429), any
///     other HTTP status, a network failure (with Qt's error text), a
///     refused answer (with parseRelease's reason), or a Config that
///     isn't usable.
/// `manual` says whether the user asked (Help › Check for Updates), so
/// the UI can stay quiet about an automatic check's failure.
class Checker : public QObject {
    Q_OBJECT
public:
    explicit Checker(Config config, QObject* parent = nullptr);
    /// Aborts a check in flight; no signal follows.
    ~Checker() override;

    const Config& config() const;

    /// Starts a check. While one runs, this starts nothing and only
    /// upgrades it to manual (check(true) during an automatic one);
    /// check(false) never downgrades a manual one.
    void check(bool manual);
    bool busy() const;

signals:
    void upToDate(const gitbolt::update::Version& latest, bool manual);
    void available(const gitbolt::update::Release& release, bool manual);
    void failed(const QString& message, bool manual);

private:
    void onFinished(QNetworkReply* reply);
    void report(QNetworkReply* reply, bool manual);

    Config config_;
    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    bool busy_ = false;
    bool manual_ = false;
    bool tooLarge_ = false;
};

} // namespace gitbolt::update
