#include "telemetry/GlitchTipSink.h"

namespace gitbolt::telemetry {

QNetworkRequest GlitchTipSink::request(const Config& config, const QString& userAgent) {
    QNetworkRequest request = jsonPostRequest(config.storeUrl, config, userAgent);
    request.setRawHeader(QByteArrayLiteral("X-Sentry-Auth"),
                         QStringLiteral("Sentry sentry_version=7, sentry_client=gitbolt/%1, "
                                        "sentry_key=%2")
                             .arg(config.appVersion, config.glitchTipKey)
                             .toUtf8());
    return request;
}

} // namespace gitbolt::telemetry
