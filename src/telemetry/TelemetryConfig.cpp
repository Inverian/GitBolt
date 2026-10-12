#include "telemetry/TelemetryConfig.h"

#include <QRegularExpression>

namespace gitbolt::telemetry {

Config Config::fromEndpoints(const QString& host, const QString& clientId,
                             const QString& clientSecret, const QString& dsnKey,
                             const QString& project, const QString& version,
                             const QString& dir) {
    Config config;
    config.trackUrl = QUrl(host + QStringLiteral("/track"));
    config.openPanelClientId = clientId;
    config.openPanelSecret = clientSecret;
    config.storeUrl = QUrl(host + QStringLiteral("/api/%1/store/").arg(project));
    config.glitchTipKey = dsnKey;
    config.appVersion = version;
    config.queueDir = dir;
    return config;
}

bool Config::isComplete() const {
    static const QRegularExpression release(
        QStringLiteral("^\\d+\\.\\d+\\.\\d+([-+][0-9A-Za-z.-]+)?$"));
    static const QRegularExpression storePath(QStringLiteral("^/api/[0-9]+/store/$"));
    return isAllowedUrl(trackUrl) && trackUrl.path() == QLatin1String("/track")
           && isAllowedUrl(storeUrl) && storePath.match(storeUrl.path()).hasMatch()
           && !openPanelClientId.isEmpty() && !openPanelSecret.isEmpty()
           && !glitchTipKey.isEmpty() && !queueDir.isEmpty()
           && release.match(appVersion).hasMatch();
}

bool Config::isAllowedUrl(const QUrl& url) {
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty())
        return false;
    if (url.scheme() == QLatin1String("https"))
        return true;
    // QUrl gives "[::1]" back as "::1".
    return url.scheme() == QLatin1String("http")
           && (url.host() == QLatin1String("127.0.0.1")
               || url.host() == QLatin1String("::1"));
}

} // namespace gitbolt::telemetry
