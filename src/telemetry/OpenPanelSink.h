#pragma once

#include "telemetry/TelemetryConfig.h"
#include "telemetry/Transport.h"

#include <QNetworkRequest>

namespace gitbolt::telemetry {

/// POST <host>/track, one event per request, authenticated with the
/// project client's id and secret (requests with the secret skip
/// OpenPanel's bot detection). No Origin header: there is no browser.
/// The client is write-only and its secret ships in every binary
/// anyway; it guards nothing but the ingest itself.
class OpenPanelSink {
public:
    static QNetworkRequest request(const Config& config, const QString& userAgent);
    static Outcome classify(int httpStatus, QNetworkReply::NetworkError error) {
        return telemetry::classify(httpStatus, error);
    }
};

} // namespace gitbolt::telemetry
