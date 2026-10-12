#include "telemetry/Transport.h"

namespace gitbolt::telemetry {

namespace {

// The request never left this machine, or reached nothing that
// answers: it says nothing about the record.
bool isUnreachable(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyConnectionRefusedError:
        return true;
    default:
        return false;
    }
}

} // namespace

Outcome classify(int httpStatus, QNetworkReply::NetworkError error) {
    if (httpStatus >= 200 && httpStatus < 300)
        return Outcome::Sent;
    if (httpStatus == 408 || httpStatus == 429 || (httpStatus >= 500 && httpStatus < 600))
        return Outcome::Retry;
    if (httpStatus > 0)
        return Outcome::Drop;
    return isUnreachable(error) ? Outcome::Unreachable : Outcome::Retry;
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
