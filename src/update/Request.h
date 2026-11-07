#pragma once

#include "update/UpdateConfig.h"

#include <QNetworkRequest>
#include <QUrl>

namespace gitbolt::update {

/// Internal to gitbolt_update (it needs QtNetwork, which the library
/// links PRIVATE): the policy the feed request and the downloads
/// share.
///   - User-Agent from the Config: GitHub's API refuses requests
///     without one.
///   - Accept-Language "*" and Accept-Encoding "identity", set on
///     purpose. Left unset, Qt sends the system locale with its region,
///     which GitHub has no need for, and asks for gzip, which a
///     download's byte count and hash must not depend on.
///   - The transfer timeout (Qt has none by default).
///   - No cookies and no cache: one request can't be linked to the
///     next, and a stale answer can't stand in for a fresh one.
///   - No redirects (ManualRedirectPolicy). Downloader overrides that
///     with its own checked policy; the feed never follows one.
/// TLS is Qt's default: certificates are checked, and no error is ever
/// ignored (nothing connects sslErrors).
QNetworkRequest updateRequest(const QUrl& url, const Config& config);

} // namespace gitbolt::update
