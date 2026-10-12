#include "telemetry/OpenPanelSink.h"

namespace gitbolt::telemetry {

QNetworkRequest OpenPanelSink::request(const Config& config, const QString& userAgent) {
    QNetworkRequest request = jsonPostRequest(config.trackUrl, config, userAgent);
    request.setRawHeader(QByteArrayLiteral("openpanel-client-id"),
                         config.openPanelClientId.toUtf8());
    request.setRawHeader(QByteArrayLiteral("openpanel-client-secret"),
                         config.openPanelSecret.toUtf8());
    return request;
}

} // namespace gitbolt::telemetry
