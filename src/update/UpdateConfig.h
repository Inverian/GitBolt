#pragma once

#include "update/Version.h"

#include <QDateTime>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace gitbolt::update {

/// The official feed and repository. Only main() of an official build
/// (GITBOLT_UPDATES_ENABLED, the app target only) puts them into a
/// Config; no library source uses them, so neither the library nor a
/// test binary that doesn't name them carries the address. A test
/// session never uses them (GITBOLT_TEST_UPDATE_FEED, loopback only).
inline constexpr char kOfficialFeed[] = "https://api.github.com/repos/Inverian/GitBolt/releases/latest";
inline constexpr char kOfficialRepo[] = "Inverian/GitBolt";
/// How often the automatic check asks, at most.
inline constexpr int kAutoCheckHours = 24;

/// Everything the checker and the downloader need from outside: where
/// to ask, which repository's releases count, which version is
/// running, how to introduce itself, where downloads may come from and
/// go to, and the limits. A plain struct, so the library never reads
/// settings or the environment: main() fills it (official feed, or a
/// test session's loopback stub), and the tests fill it with their own
/// stub and short timeouts.
///
/// Why the allowlists: GitBolt has no Developer ID or Authenticode
/// signature to check, so the updater can only check where a file came
/// from (HTTPS, from GitHub's own hosts, Qt's default certificate
/// checks, TLS errors never ignored) and that it arrived intact (size
/// and SHA-256, Downloader.h). That is the trust a user has who
/// downloads the release by hand, and no more: the release's Sigstore
/// attestations would need a Sigstore verifier, which GitBolt doesn't
/// have, so they are not checked.
struct Config {
    /// The releases/latest endpoint. https://api.github.com only, or,
    /// with allowLoopback, plain http to 127.0.0.1 or [::1].
    QUrl feedUrl;
    /// "Owner/Name": a release's page must be under
    /// https://github.com/<repo>/releases/.
    QString repo;
    /// The running version (GITBOLT_TEST_APP_VERSION in a test session).
    Version current;
    /// "GitBolt/<version> (<OS token>)", on every request: GitHub's API
    /// refuses a request without one. main() builds it with
    /// telemetry::userAgent, so both say the same; printable ASCII only.
    QString userAgent;
    /// Hosts a download, and every redirect on its way, may use (https).
    QStringList downloadHosts{QStringLiteral("github.com"), QStringLiteral("objects.githubusercontent.com"),
                              QStringLiteral("release-assets.githubusercontent.com")};
    /// Tests: plain http to 127.0.0.1 or [::1], any port, is allowed
    /// for the feed and for downloads, on top of the rules above.
    bool allowLoopback = false;
    /// <QStandardPaths::CacheLocation>/updates, absolute. Downloads go
    /// into <downloadDir>/<version>/ unless the caller names a folder.
    QString downloadDir;
    /// QNetworkRequest::setTransferTimeout: how long a request may go
    /// without a byte moving. Not a cap on a long download's duration.
    int requestTimeoutMs = 15000;
    qint64 maxDownloadBytes = 512LL * 1024 * 1024;

    /// True only when every field makes sense: an allowed feed URL, a
    /// repository of the form Owner/Name, a version other than 0.0.0,
    /// a printable-ASCII User-Agent, an absolute download folder, and
    /// positive limits. Checker and Downloader refuse to work with
    /// anything else, so a half-filled Config fails closed.
    bool isUsable() const;
    /// https to api.github.com (default port, no user info), or the
    /// loopback rule when allowLoopback is on.
    bool isAllowedFeedUrl(const QUrl& url) const;
    /// https to one of downloadHosts (default port, no user info), or
    /// the loopback rule when allowLoopback is on.
    bool isAllowedDownloadUrl(const QUrl& url) const;
};

/// Plain http to 127.0.0.1 or [::1], any port, no user info: the only
/// URLs a test session may point the updater at (main() checks
/// GITBOLT_TEST_UPDATE_FEED with this).
bool isLoopbackHttpUrl(const QUrl& url);

/// Whether the automatic check should ask now: true if `lastCheckUtc`
/// is invalid (never checked, or an unreadable setting), lies more
/// than an hour in the future (the clock was set back, which would
/// otherwise silence the check until the clock caught up), or is at
/// least `hours` before `nowUtc`. An invalid `nowUtc` counts as due:
/// the hourly timer bounds how often that can ask.
bool dueForAutoCheck(const QDateTime& lastCheckUtc, const QDateTime& nowUtc, int hours = kAutoCheckHours);

/// GITBOLT_UPDATE_CHECK is 0, false, off or no (case-insensitive,
/// surrounding spaces ignored): the user turned automatic checks off
/// through the environment. A manual check still asks.
bool updateCheckOptedOut(const QProcessEnvironment& env);

} // namespace gitbolt::update
