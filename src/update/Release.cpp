#include "update/Release.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace gitbolt::update {

namespace {

ParsedRelease refuse(const QString& why) {
    return ParsedRelease{std::nullopt, why};
}

// GitHub's text inside one of our sentences: one line, and short, so a
// tag of a megabyte can't take over the dialog.
QString shortened(const QString& text) {
    const QString line = text.simplified();
    return line.size() > 40 ? line.left(40) + QChar(0x2026) : line;
}

bool isLowerOrUpperHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// What may follow https://github.com/<repo>/releases/ in a release's
// page: GitHub's "tag/v1.2.3". No "%", "?", "#", "@" or ":", so the
// string and the URL Qt makes of it can't disagree, and no ".."
// segment, so it can't climb out of the releases.
bool isPlainPageRest(const QString& rest) {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9._~+/-]*$"));
    if (!plain.match(rest).hasMatch())
        return false;
    const QStringList segments = rest.split(u'/');
    return std::none_of(segments.cbegin(), segments.cend(),
                        [](const QString& segment) { return segment == QLatin1String(".."); });
}

std::optional<ReleaseAsset> assetFrom(const QJsonValue& value) {
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    ReleaseAsset asset;
    asset.name = object.value(QLatin1String("name")).toString();
    asset.downloadUrl = QUrl(object.value(QLatin1String("browser_download_url")).toString(), QUrl::StrictMode);
    asset.size = object.value(QLatin1String("size")).toInteger(-1);
    if (asset.name.isEmpty() || !asset.downloadUrl.isValid() || asset.downloadUrl.isRelative() || asset.size < 0)
        return std::nullopt;
    const QString digest = object.value(QLatin1String("digest")).toString();
    if (digest.startsWith(QLatin1String("sha256:"), Qt::CaseInsensitive))
        asset.sha256 = digest.mid(7).toLower().toLatin1();
    return asset;
}

} // namespace

const ReleaseAsset* Release::asset(const QString& assetName) const {
    const auto it = std::find_if(assets.cbegin(), assets.cend(),
                                 [&](const ReleaseAsset& asset) { return asset.name == assetName; });
    return it == assets.cend() ? nullptr : &*it;
}

ParsedRelease parseRelease(const QByteArray& json, const QString& repo) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return refuse(QCoreApplication::translate("gitbolt::update::Release", "it isn't JSON"));
    if (!document.isObject())
        return refuse(QCoreApplication::translate("gitbolt::update::Release", "it isn't a release object"));
    const QJsonObject object = document.object();

    // Both flags must be there and false: releases/latest never gives
    // a draft or a pre-release, so anything else isn't that endpoint.
    const QJsonValue draft = object.value(QLatin1String("draft"));
    if (!draft.isBool())
        return refuse(QCoreApplication::translate("gitbolt::update::Release", "it doesn't say whether it is a draft"));
    if (draft.toBool())
        return refuse(QCoreApplication::translate("gitbolt::update::Release", "it is a draft"));
    const QJsonValue prerelease = object.value(QLatin1String("prerelease"));
    if (!prerelease.isBool())
        return refuse(
            QCoreApplication::translate("gitbolt::update::Release", "it doesn't say whether it is a pre-release"));
    if (prerelease.toBool())
        return refuse(QCoreApplication::translate("gitbolt::update::Release", "it is a pre-release"));

    Release release;
    release.tag = object.value(QLatin1String("tag_name")).toString();
    const std::optional<Version> version =
        release.tag.startsWith(u'v') ? Version::parse(release.tag) : std::nullopt;
    if (!version) {
        if (release.tag.isEmpty())
            return refuse(QCoreApplication::translate("gitbolt::update::Release", "it has no tag"));
        return refuse(QCoreApplication::translate("gitbolt::update::Release", "its tag “%1” isn't v<x.y.z>")
                          .arg(shortened(release.tag)));
    }
    release.version = *version;

    const QString prefix = QStringLiteral("https://github.com/%1/releases/").arg(repo);
    const QString page = object.value(QLatin1String("html_url")).toString();
    release.pageUrl = QUrl(page, QUrl::StrictMode);
    if (repo.isEmpty() || !page.startsWith(prefix) || !isPlainPageRest(page.mid(prefix.size()))
        || !release.pageUrl.isValid() || release.pageUrl.host() != QLatin1String("github.com"))
        return refuse(
            QCoreApplication::translate("gitbolt::update::Release", "its page isn't under %1").arg(prefix));

    release.name = object.value(QLatin1String("name")).toString();
    if (release.name.trimmed().isEmpty())
        release.name = release.tag;
    release.notes = object.value(QLatin1String("body")).toString();

    QSet<QString> names;
    const QJsonArray assets = object.value(QLatin1String("assets")).toArray();
    for (const QJsonValue& value : assets) {
        std::optional<ReleaseAsset> asset = assetFrom(value);
        if (!asset)
            continue;
        if (names.contains(asset->name))
            return refuse(QCoreApplication::translate("gitbolt::update::Release", "it lists “%1” twice")
                              .arg(shortened(asset->name)));
        names.insert(asset->name);
        release.assets.push_back(std::move(*asset));
    }
    return ParsedRelease{std::move(release), QString()};
}

QByteArray checksumFor(const QByteArray& sha256SumsFile, const QString& assetName) {
    if (assetName.isEmpty())
        return {};
    const QByteArray name = assetName.toUtf8();
    QByteArray found;
    for (QByteArray line : sha256SumsFile.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        // "<64 hex><space><space or *><name>": text or binary mode.
        if (line.size() != 66 + name.size() || line.at(64) != ' ' || (line.at(65) != ' ' && line.at(65) != '*')
            || !line.endsWith(name))
            continue;
        const QByteArray hex = line.left(64);
        if (!std::all_of(hex.cbegin(), hex.cend(), isLowerOrUpperHex))
            continue;
        const QByteArray lower = hex.toLower();
        if (!found.isEmpty() && found != lower)
            return {};   // two answers: neither can be trusted
        found = lower;
    }
    return found;
}

} // namespace gitbolt::update
