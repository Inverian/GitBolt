#pragma once

#include "telemetry/TelemetryConfig.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace gitbolt::telemetry {

/// What a finished request means for its record.
enum class Outcome {
    Sent,    ///< drop it from the queue, send the next after the pace
    Retry,   ///< count an attempt, end this flush cycle
    Drop,    ///< drop it, send the next: retrying can't help
};

/// The rules both sinks share:
///   - 2xx: Sent.
///   - 408, 429, 5xx: Retry.
///   - any other status (3xx — redirects are never followed —, other
///     4xx: bad key, malformed body, 413): Drop.
///   - no status at all: Retry. The request got no answer: refused,
///     DNS, TLS, TimeoutError (the transfer timeout), or an
///     OperationCanceledError the service didn't start (its own aborts
///     never get here). maxAttempts bounds every retry.
Outcome classify(int httpStatus, QNetworkReply::NetworkError error);

/// A POST request to `url` with the transport policy both sinks share:
/// Content-Type application/json, the given User-Agent, an explicit
/// Accept-Language "*" and Accept-Encoding "identity" (left unset, Qt
/// fills them in itself, Accept-Language from the system locale with
/// its region), the transfer timeout (Qt has none by default), no
/// redirects (a misrouted host can't bounce the data elsewhere), no
/// cookies (Cloudflare's can't link requests) and no cache. Whatever
/// headers this sets plus the sink's own are all a request carries
/// beyond Host, Content-Length and Connection; the tests' stub fails
/// on any other.
QNetworkRequest jsonPostRequest(const QUrl& url, const Config& config, const QString& userAgent);

} // namespace gitbolt::telemetry
