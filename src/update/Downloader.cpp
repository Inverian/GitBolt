#include "update/Downloader.h"

#include "update/Request.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>

#include <filesystem>
#include <system_error>
#include <utility>

namespace gitbolt::update {

namespace {

// SHA256SUMS.txt lists three installers and three symbols zips in
// about 600 bytes.
constexpr qint64 kMaxChecksumsBytes = 64 * 1024;
// github.com answers browser_download_url with one redirect to its
// asset host; a few more leave room without allowing a loop.
constexpr int kMaxRedirects = 5;

// A name to put into a folder as it is: no separator, no "..", no
// leading dot (that is the .part's), nothing a shell or a file system
// treats specially.
bool isPlainFileName(const QString& name) {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]*$"));
    return plain.match(name).hasMatch() && !name.contains(QLatin1String(".."));
}

// Scheme, host and port: enough to say where, without a path or query
// from GitHub's answer.
QString origin(const QUrl& url) {
    return url.toDisplayString(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
}

} // namespace

Downloader::Downloader(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

Downloader::~Downloader() {
    cancel();
}

const Config& Downloader::config() const {
    return config_;
}

bool Downloader::busy() const {
    return busy_;
}

Failure Downloader::failure() const {
    return failure_;
}

void Downloader::start(const Release& release, const QString& assetName) {
    if (busy_)
        return;
    busy_ = true;
    ++run_;
    failure_ = Failure::None;
    const Problem why = prepare(release, assetName);
    if (!why.isEmpty()) {
        failLater(why);
        return;
    }
    fetchChecksums();
}

void Downloader::cancel() {
    if (!busy_)
        return;
    dropReply();
    discardPart();
    reset();
}

Downloader::Problem Downloader::prepare(const Release& release, const QString& assetName) {
    const QString sumsName = QString::fromLatin1(kChecksumsAsset);
    // What GitBolt won't ask for at all.
    const auto refused = [](const QString& message) { return Problem{Failure::Refused, message}; };
    if (!config_.isUsable())
        return refused(tr("Updates aren't set up in this copy of GitBolt."));
    if (!isPlainFileName(assetName))
        return refused(tr("“%1” isn't a file GitBolt downloads.").arg(assetName));
    const ReleaseAsset* asset = release.asset(assetName);
    if (!asset)
        return refused(tr("GitBolt %1 has no %2.").arg(release.version.toString(), assetName));
    const ReleaseAsset* sums = release.asset(sumsName);
    if (!sums)
        return refused(tr("GitBolt %1 has no %2, so its download can't be checked.")
                           .arg(release.version.toString(), sumsName));
    if (asset->size <= 0)
        return refused(tr("GitHub lists %1 as empty.").arg(assetName));
    if (asset->size > config_.maxDownloadBytes)
        return refused(tr("%1 is larger than GitBolt downloads (%2 MiB).")
                           .arg(assetName)
                           .arg(config_.maxDownloadBytes / (1024 * 1024)));
    if (sums->size <= 0 || sums->size > kMaxChecksumsBytes)
        return refused(tr("GitHub lists %1 with a size it can't have.").arg(sumsName));
    for (const QUrl& url : {sums->downloadUrl, asset->downloadUrl}) {
        if (!config_.isAllowedDownloadUrl(url))
            return refused(tr("GitBolt doesn't download from %1.").arg(origin(url)));
    }

    const QString target = QDir(config_.downloadDir).filePath(release.version.toString());
    if (!QDir::isAbsolutePath(target))
        return refused(tr("GitBolt can't download into %1.").arg(QDir::toNativeSeparators(target)));
    if (!QDir().mkpath(target))
        return {Failure::DiskError, tr("Couldn't create the folder %1.").arg(QDir::toNativeSeparators(target))};

    asset_ = *asset;
    checksums_ = *sums;
    finalPath_ = QDir(target).filePath(assetName);
    partPath_ = QDir(target).filePath(QLatin1Char('.') + assetName + QStringLiteral(".part"));
    return {};
}

QNetworkReply* Downloader::get(const QUrl& url) {
    if (!network_) {
        network_ = new QNetworkAccessManager(this);
        network_->setRedirectPolicy(QNetworkRequest::UserVerifiedRedirectPolicy);
    }
    QNetworkRequest request = updateRequest(url, config_);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/octet-stream"));
    // UserVerified, not NoLessSafeRedirectPolicy: with that one Qt
    // follows on its own, and only an abort() from redirected() stops
    // it, which Qt 6.11 honours but documents nowhere. UserVerified is
    // the documented veto: nothing goes to the new URL until
    // redirectAllowed(). The allowlist (https only, but for the tests'
    // loopback) is at least as strict as "no less safe".
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(kMaxRedirects);

    QNetworkReply* reply = network_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::redirected, this, [this, reply](const QUrl& to) {
        if (reply != reply_)
            return;
        if (!config_.isAllowedDownloadUrl(to)) {
            fail({Failure::Refused,
                  tr("The download was sent on to %1, which GitBolt doesn't download from.").arg(origin(to))});
            return;
        }
        emit reply->redirectAllowed();
    });
    return reply;
}

void Downloader::fetchChecksums() {
    checksumsBody_.clear();
    QNetworkReply* reply = get(checksums_.downloadUrl);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply != reply_)
            return;
        // A redirect's body, or an error page's: never part of the file.
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            reply->readAll();
            return;
        }
        checksumsBody_ += reply->readAll();
        if (checksumsBody_.size() > checksums_.size)
            fail({Failure::SizeMismatch,
                  tr("%1 is larger than GitHub lists.").arg(QString::fromLatin1(kChecksumsAsset))});
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onChecksumsFinished(reply); });
}

void Downloader::onChecksumsFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply != reply_)
        return;
    reply_ = nullptr;
    const QString sumsName = QString::fromLatin1(kChecksumsAsset);
    if (const Problem why = replyProblem(reply, sumsName); !why.isEmpty()) {
        fail(why);
        return;
    }
    checksumsBody_ += reply->readAll();
    if (checksumsBody_.size() != checksums_.size) {
        fail({Failure::SizeMismatch, tr("%1 arrived with %2 bytes; GitHub lists %3.")
                                         .arg(sumsName)
                                         .arg(checksumsBody_.size())
                                         .arg(checksums_.size)});
        return;
    }
    expected_ = checksumFor(checksumsBody_, asset_.name);
    if (expected_.isEmpty()) {
        fail({Failure::ChecksumMissing, tr("%1 has no checksum for %2.").arg(sumsName, asset_.name)});
        return;
    }
    // Both are GitHub's word for the same file: if they disagree, no
    // download can match both, so don't fetch it.
    if (!asset_.sha256.isEmpty() && asset_.sha256 != expected_) {
        fail({Failure::ChecksumConflict,
              tr("GitHub's checksum for %1 differs from the one in %2.").arg(asset_.name, sumsName)});
        return;
    }
    fetchAsset();
}

void Downloader::fetchAsset() {
    // A file of our own, made here: whatever has the .part's name goes
    // first (a leftover, or a link someone planted; remove() unlinks a
    // link itself, never what it points to), and the open is NewOnly
    // (O_CREAT|O_EXCL, CREATE_NEW on Windows), which fails on any
    // existing name, a link included, rather than follow it. If the name
    // can't be freed, the download fails rather than write anywhere else.
    QFile::remove(partPath_);
    file_ = std::make_unique<QFile>(partPath_);
    if (!file_->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        const QString why = file_->errorString();
        // Not discardPart's remove(): what is there isn't ours.
        file_.reset();
        partPath_.clear();
        fail({Failure::DiskError, tr("Couldn't save %1: %2.").arg(asset_.name, why)});
        return;
    }
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    received_ = 0;
    QNetworkReply* reply = get(asset_.downloadUrl);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply != reply_)
            return;
        const qint64 before = received_;
        if (const Problem why = consume(reply); !why.isEmpty()) {
            fail(why);
            return;
        }
        if (received_ != before)
            emit progress(received_, asset_.size);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onAssetFinished(reply); });
}

Downloader::Problem Downloader::consume(QNetworkReply* reply) {
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        reply->readAll();
        return {};
    }
    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty())
        return {};
    if (received_ + chunk.size() > asset_.size)
        return {Failure::SizeMismatch,
                tr("%1 is larger than GitHub lists (%2 bytes).").arg(asset_.name).arg(asset_.size)};
    if (file_->write(chunk) != chunk.size())
        return {Failure::DiskError, tr("Couldn't save %1: %2.").arg(asset_.name, file_->errorString())};
    hash_->addData(chunk);
    received_ += chunk.size();
    return {};
}

void Downloader::onAssetFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply != reply_)
        return;
    reply_ = nullptr;
    if (const Problem why = replyProblem(reply, asset_.name); !why.isEmpty()) {
        fail(why);
        return;
    }
    if (const Problem why = consume(reply); !why.isEmpty()) {
        fail(why);
        return;
    }
    if (!file_->flush()) {
        fail({Failure::DiskError, tr("Couldn't save %1: %2.").arg(asset_.name, file_->errorString())});
        return;
    }
    file_->close();
    file_.reset();
    if (received_ != asset_.size) {
        fail({Failure::SizeMismatch,
              tr("%1 arrived with %2 bytes; GitHub lists %3.").arg(asset_.name).arg(received_).arg(asset_.size)});
        return;
    }
    const QByteArray actual = hash_->result().toHex();
    if (actual != expected_) {
        fail({Failure::ChecksumMismatch, tr("%1 doesn't match its checksum in %2, so it was deleted.")
                                             .arg(asset_.name, QString::fromLatin1(kChecksumsAsset))});
        return;
    }
    if (!asset_.sha256.isEmpty() && actual != asset_.sha256) {
        fail({Failure::ChecksumMismatch,
              tr("%1 doesn't match the checksum GitHub lists for it, so it was deleted.").arg(asset_.name)});
        return;
    }

    // Into place, by the system's rename alone (rename(2); MoveFileExW
    // with MOVEFILE_REPLACE_EXISTING on Windows): it replaces whatever
    // has the final name, an earlier copy or a link (the link itself),
    // in one step, and never writes through a link. Not QFile::rename: it
    // never replaces, and when the rename fails it copies instead,
    // through a link at the final name to wherever it points.
    std::error_code error;
    std::filesystem::rename(std::filesystem::path(partPath_.toStdU16String()),
                            std::filesystem::path(finalPath_.toStdU16String()), error);
    if (error) {
        fail({Failure::DiskError,
              tr("Couldn't move %1 into place: %2.").arg(asset_.name, QString::fromLocal8Bit(error.message()))});
        return;
    }
    const QString path = finalPath_;
    reset();
    emit finished(path);
}

Downloader::Problem Downloader::replyProblem(QNetworkReply* reply, const QString& name) const {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status > 0 && status != 200)
        return {Failure::HttpError, tr("Couldn't download %1: GitHub answered HTTP %2.").arg(name).arg(status)};
    // No status: no answer. A 200 with an error: cut off midway.
    if (status != 200 || reply->error() != QNetworkReply::NoError)
        return {Failure::NetworkError, tr("Couldn't download %1: %2.").arg(name, reply->errorString())};
    return {};
}

void Downloader::failLater(const Problem& problem) {
    QTimer::singleShot(0, this, [this, run = run_, problem] {
        if (run == run_)
            fail(problem);
    });
}

void Downloader::fail(const Problem& problem) {
    dropReply();
    discardPart();
    reset();
    failure_ = problem.failure;
    emit failed(problem.message);
}

void Downloader::dropReply() {
    // Disconnect BEFORE abort(): it emits finished() synchronously.
    if (QNetworkReply* reply = std::exchange(reply_, nullptr)) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void Downloader::discardPart() {
    if (file_) {
        file_->close();
        file_.reset();
    }
    if (!partPath_.isEmpty())
        QFile::remove(partPath_);
}

void Downloader::reset() {
    busy_ = false;
    ++run_;
    asset_ = {};
    checksums_ = {};
    partPath_.clear();
    finalPath_.clear();
    checksumsBody_.clear();
    expected_.clear();
    hash_.reset();
    received_ = 0;
}

void removeOldDownloads(const QString& downloadDir, const Version& current) {
    if (downloadDir.isEmpty() || !QDir::isAbsolutePath(downloadDir))
        return;
    const QFileInfoList entries =
        QDir(downloadDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink() || entry.isJunction())
            continue;
        const std::optional<Version> version = Version::parse(entry.fileName());
        if (!version || version->toString() != entry.fileName() || *version > current)
            continue;
        QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

} // namespace gitbolt::update
