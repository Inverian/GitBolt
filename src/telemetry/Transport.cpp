#include "telemetry/Transport.h"

namespace gitbolt::telemetry {

Outcome classify(int httpStatus, QNetworkReply::NetworkError error) {
    Q_UNUSED(error);   // every no-status error is a Retry; see the header
    if (httpStatus >= 200 && httpStatus < 300)
        return Outcome::Sent;
    if (httpStatus == 408 || httpStatus == 429 || (httpStatus >= 500 && httpStatus < 600))
        return Outcome::Retry;
    if (httpStatus > 0)
        return Outcome::Drop;
    return Outcome::Retry;
}

QNetworkRequest jsonPostRequest(const QUrl& url, const Config& config, const QString& userAgent) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    request.setRawHeader(QByteArrayLiteral("Accept-Language"), QByteArrayLiteral("*"));
    request.setRawHeader(QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity"));
    request.setTransferTimeout(config.transferTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    return request;
}

} // namespace gitbolt::telemetry
