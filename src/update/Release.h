#pragma once

#include "update/Version.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>
#include <vector>

namespace gitbolt::update {

/// The release file that lists every installer's SHA-256, made by
/// `sha256sum -- * > SHA256SUMS.txt` in the release job. Required: a
/// release without it can be shown, but not installed.
inline constexpr char kChecksumsAsset[] = "SHA256SUMS.txt";

/// One file of a release, as GitHub's API lists it.
struct ReleaseAsset {
    QString name;
    /// browser_download_url: github.com, which redirects to its asset
    /// host. Not checked here; Downloader checks it and every redirect.
    QUrl downloadUrl;
    /// Bytes, from the API; the download must be exactly this long.
    qint64 size = 0;
    /// Lower-case hex from the API's "digest" ("sha256:<hex>"), or
    /// empty when the API gave none (older releases) or another
    /// algorithm. A malformed sha256 value is kept as it came, so it
    /// can never match a real checksum: the download then fails.
    QByteArray sha256;
};

/// A release the updater may offer. Everything in it but `version`
/// and `pageUrl` is GitHub's text, untrusted: the dialog shows `name`
/// and `notes` as plain text, renders no markdown or HTML, and follows
/// no link in them.
struct Release {
    Version version;
    QString tag;          ///< "v<x.y.z>"
    QUrl pageUrl;         ///< html_url, under https://github.com/<repo>/releases/
    QString name;         ///< the release's title; the tag when GitHub gave none
    QString notes;        ///< the body (markdown source), shown as written
    std::vector<ReleaseAsset> assets;

    /// The asset named exactly `assetName`, or nullptr.
    const ReleaseAsset* asset(const QString& assetName) const;
};

/// parseRelease's answer: a release, or why the answer isn't one.
struct ParsedRelease {
    std::optional<Release> release;
    /// Short, lower-case, no full stop ("it is a draft"), for the log
    /// and for "GitHub's answer wasn't a GitBolt release: <error>."
    QString error;
};

/// Reads GitHub's releases/latest answer. Refuses (error set, no
/// release) anything that isn't a JSON object, a draft or a
/// pre-release (both flags must be present and false), a tag other
/// than v<x.y.z> (Version::parse, with the "v"), and a page (html_url)
/// outside https://github.com/<repo>/releases/. Asset entries missing
/// a name, a valid URL or a size are skipped; an asset name listed
/// twice refuses the release, as GitHub never does that and a reader
/// couldn't tell which one is meant.
ParsedRelease parseRelease(const QByteArray& json, const QString& repo);

/// The lower-case hex SHA-256 that `sha256SumsFile` (sha256sum's
/// output: "<64 hex>  <name>" or "<64 hex> *<name>", one per line, LF
/// or CRLF) gives for exactly `assetName`, or empty: no such line, a
/// malformed one, or two lines for the name that disagree. Escaped
/// lines ("\<hex>  <name with a backslash or newline>") never match,
/// as no GitBolt asset has such a name.
QByteArray checksumFor(const QByteArray& sha256SumsFile, const QString& assetName);

} // namespace gitbolt::update
