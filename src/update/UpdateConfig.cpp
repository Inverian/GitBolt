#include "update/UpdateConfig.h"

#include <QDir>
#include <QRegularExpression>

namespace gitbolt::update {

namespace {

// https on the default port, no user info, a host from `hosts`. QUrl
// gives the host back lower-cased, and a trailing dot ("github.com.")
// is kept, so it never matches.
bool isHttpsTo(const QUrl& url, const QStringList& hosts) {
    return url.isValid() && url.scheme() == QLatin1String("https") && url.userInfo().isEmpty()
           && (url.port() == -1 || url.port() == 443) && hosts.contains(url.host(), Qt::CaseInsensitive);
}

bool isPrintableAscii(const QString& text) {
    for (const QChar c : text) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e)
            return false;
    }
    return true;
}

} // namespace

bool isLoopbackHttpUrl(const QUrl& url) {
    // QUrl gives "[::1]" back as "::1".
    return url.isValid() && url.scheme() == QLatin1String("http") && url.userInfo().isEmpty()
           && (url.host() == QLatin1String("127.0.0.1") || url.host() == QLatin1String("::1"));
}

bool Config::isAllowedFeedUrl(const QUrl& url) const {
    if (allowLoopback && isLoopbackHttpUrl(url))
        return true;
    return isHttpsTo(url, {QStringLiteral("api.github.com")});
}

bool Config::isAllowedDownloadUrl(const QUrl& url) const {
    if (allowLoopback && isLoopbackHttpUrl(url))
        return true;
    return isHttpsTo(url, downloadHosts);
}

bool Config::isUsable() const {
    // GitHub's own rules for owner and repository names, roughly; what
    // matters is that it can't carry a slash, a dot-dot or a query into
    // the page prefix parseRelease checks against.
    static const QRegularExpression repoPattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_.-]+$"));
    return isAllowedFeedUrl(feedUrl) && repoPattern.match(repo).hasMatch()
           && !repo.contains(QLatin1String("..")) && current != Version{} && !userAgent.isEmpty()
           && isPrintableAscii(userAgent) && !downloadDir.isEmpty() && QDir::isAbsolutePath(downloadDir)
           && requestTimeoutMs > 0 && maxDownloadBytes > 0;
}

bool dueForAutoCheck(const QDateTime& lastCheckUtc, const QDateTime& nowUtc, int hours) {
    if (!lastCheckUtc.isValid() || !nowUtc.isValid())
        return true;
    const qint64 elapsed = lastCheckUtc.secsTo(nowUtc);
    if (elapsed < -3600)
        return true;   // the clock went back: don't wait for it to catch up
    return elapsed >= qint64(hours) * 3600;
}

bool updateCheckOptedOut(const QProcessEnvironment& env) {
    const QString value = env.value(QStringLiteral("GITBOLT_UPDATE_CHECK")).trimmed();
    for (const char* off : {"0", "false", "off", "no"}) {
        if (value.compare(QLatin1String(off), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

} // namespace gitbolt::update
