#include "update/Request.h"

namespace gitbolt::update {

QNetworkRequest updateRequest(const QUrl& url, const Config& config) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, config.userAgent);
    request.setRawHeader(QByteArrayLiteral("Accept-Language"), QByteArrayLiteral("*"));
    request.setRawHeader(QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity"));
    request.setTransferTimeout(config.requestTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    return request;
}

} // namespace gitbolt::update
