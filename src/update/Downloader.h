#pragma once

#include "update/Release.h"
#include "update/UpdateConfig.h"
#include "update/Version.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QCryptographicHash;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace gitbolt::update {

/// Downloads one installer of a release and checks it, on the GUI
/// thread, one download at a time.
///
/// What a file must pass before finished() names it (see Config for
/// why these and no more):
///   - every URL on its way, the first one included, is allowed by
///     Config::isAllowedDownloadUrl (https to GitHub's hosts). Each
///     redirect is checked before it is followed
///     (UserVerifiedRedirectPolicy: Qt sends nothing to the new URL
///     until redirectAllowed()), so a refused host never sees a request;
///   - it is exactly the size the API lists, and that size is at most
///     Config::maxDownloadBytes;
///   - its SHA-256 equals the line for its name in the release's
///     SHA256SUMS.txt (fetched first, under the same rules, and
///     required), and the API's "digest" for it when there is one.
///
/// The file is written as `.<name>.part` in the target folder, hashed
/// as it arrives, and renamed to `<name>` only once it has passed: the
/// final name never holds a partial or unchecked file. The .part is
/// always a new file: anything already at its name is removed first
/// (a link itself, not what it points to) and it is created
/// exclusively. The final name is taken by the system's rename alone,
/// never a copy, which replaces whatever entry has it (a link itself),
/// so a planted link can't send the bytes elsewhere at either name. A
/// failure or cancel() deletes the .part; a file already at the final
/// name is replaced only by one that passed.
class Downloader : public QObject {
    Q_OBJECT
public:
    explicit Downloader(Config config, QObject* parent = nullptr);
    /// Cancels a download in flight; no signal follows.
    ~Downloader() override;

    const Config& config() const;

    /// Gets SHA256SUMS.txt, then `assetName` (a plain file name, as
    /// assetNameFor gives it) into `dir`, or, when `dir` is empty, into
    /// Config::downloadDir/<version>/ (created as needed). Ignored while
    /// busy(). Every outcome is a signal from the event loop, never from
    /// inside start(), with busy() already false.
    void start(const Release& release, const QString& assetName, const QString& dir = {});
    /// Stops the download and deletes its .part. Emits nothing.
    void cancel();
    bool busy() const;

signals:
    /// Bytes of the installer so far, of `total` (the API's size).
    void progress(qint64 received, qint64 total);
    /// The installer, checked, at its final name.
    void finished(const QString& filePath);
    /// A sentence for the dialog; nothing is left behind.
    void failed(const QString& message);

private:
    QString prepare(const Release& release, const QString& assetName, const QString& dir);
    QNetworkReply* get(const QUrl& url);
    void fetchChecksums();
    void onChecksumsFinished(QNetworkReply* reply);
    void fetchAsset();
    void onAssetFinished(QNetworkReply* reply);
    /// Writes and hashes what `reply` has; an error sentence, or empty.
    QString consume(QNetworkReply* reply);
    QString replyProblem(QNetworkReply* reply, const QString& name) const;
    void failLater(const QString& message);
    void fail(const QString& message);
    void dropReply();
    void discardPart();
    void reset();

    Config config_;
    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    bool busy_ = false;
    /// Bumped by every start and every end: a queued failure from an
    /// earlier run is dropped.
    quint64 run_ = 0;

    ReleaseAsset asset_;
    ReleaseAsset checksums_;
    QString partPath_;
    QString finalPath_;
    QByteArray checksumsBody_;
    QByteArray expected_;
    std::unique_ptr<QFile> file_;
    std::unique_ptr<QCryptographicHash> hash_;
    qint64 received_ = 0;
};

/// Deletes <downloadDir>/<v>/ for every v <= `current`: the installers
/// of the running version and older ones, which nothing needs once it
/// runs. Only folders named exactly like a version ("0.1.0"); anything
/// else in downloadDir, and a link or junction of any name, is left
/// alone, so a link can't make it delete what it points to.
void removeOldDownloads(const QString& downloadDir, const Version& current);

} // namespace gitbolt::update
