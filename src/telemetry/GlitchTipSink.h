#pragma once

#include "telemetry/TelemetryConfig.h"
#include "telemetry/Transport.h"

#include <QNetworkRequest>

namespace gitbolt::telemetry {

/// POST <host>/api/<project>/store/, a Sentry store event, with
/// "X-Sentry-Auth: Sentry sentry_version=7, sentry_client=gitbolt/<ver>,
/// sentry_key=<key>" (a DSN key is public by design). GlitchTip
/// rejects a repeated event_id, so a retry after a lost reply can't
/// double-count.
class GlitchTipSink {
public:
    static QNetworkRequest request(const Config& config, const QString& userAgent);
    /// The same rules as OpenPanelSink.
    static Outcome classify(int httpStatus, QNetworkReply::NetworkError error) {
        return telemetry::classify(httpStatus, error);
    }
};

} // namespace gitbolt::telemetry
