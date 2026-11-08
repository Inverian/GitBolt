//
// TestUpdateDownloader — the installer download against a loopback stub
// (HttpServer.h). Never GitHub: every URL is the stub's, which only a
// Config with allowLoopback accepts.
//
//   - success: SHA256SUMS.txt first, then the installer, checked and
//     renamed into <downloadDir>/<version>/ (or the folder given), with
//     progress against the API's size; through redirects to allowed
//     hosts too
//   - every refusal leaves nothing behind (no final file, no .part):
//     a checksum that doesn't match, the API's digest disagreeing, no
//     line in SHA256SUMS.txt, no SHA256SUMS.txt, no such asset, a size
//     other than the API's, over the cap, an HTTP error, a stall
//   - a redirect to a host not on the list is never followed: that host
//     gets no request at all; nor does a first URL not on it
//   - cancel() stops it, deletes the .part and emits nothing
//   - the final name only ever holds a checked file: never the .part's
//     bytes, and an earlier file there is replaced only by one that
//     passed
//   - the .part is always a new file: a link planted at its name is
//     removed, never written through (nor is a file created where a
//     dangling one points), and something there that can't be removed
//     fails the download
//   - removeOldDownloads: the folders of the running version and older
//     ones go, anything else stays, and a link is never followed
//

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "HttpServer.h"
#include "update/Downloader.h"
#include "update/Release.h"
#include "update/UpdateConfig.h"
#include "update/Version.h"

#include <memory>

using namespace gitbolt::update;
using gitbolt::test::HttpServer;
using gitbolt::test::Served;

namespace {

const QString kAsset = QStringLiteral("GitBolt-0.1.1-x86_64.AppImage");
const QString kSums = QString::fromLatin1(kChecksumsAsset);
const QByteArray kAssetPath = "/Inverian/GitBolt/releases/download/v0.1.1/GitBolt-0.1.1-x86_64.AppImage";
const QByteArray kSumsPath = "/Inverian/GitBolt/releases/download/v0.1.1/SHA256SUMS.txt";
const QString kUserAgent = QStringLiteral("GitBolt/0.1.0 (X11; Linux x86_64)");

// Bytes that aren't all alike, so a dropped or repeated chunk changes
// the hash.
QByteArray payload(int size) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        bytes[i] = char((i * 31 + i / 997) & 0xff);
    return bytes;
}

QByteArray sha256Hex(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// sha256sum's output for the installer, between two other installers'.
QByteArray sumsFor(const QByteArray& file) {
    return QByteArray("1").repeated(64) + "  GitBolt-0.1.1-Darwin-arm64.dmg\n" + sha256Hex(file) + "  "
           + kAsset.toLatin1() + "\n" + QByteArray("2").repeated(64) + "  gitbolt_0.1.1_amd64.deb\n";
}

ReleaseAsset* assetIn(Release& release, const QString& name) {
    for (ReleaseAsset& asset : release.assets) {
        if (asset.name == name)
            return &asset;
    }
    return nullptr;
}

QStringList entriesOf(const QString& dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

QByteArray contentsOf(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(content) == content.size();
}

} // namespace

class TestUpdateDownloader : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<HttpServer> server_;
    std::unique_ptr<QTemporaryDir> dir_;

    QString downloadDir() const { return dir_->path() + QStringLiteral("/updates"); }
    QString versionDir() const { return downloadDir() + QStringLiteral("/0.1.1"); }
    QString finalPath() const { return versionDir() + QLatin1Char('/') + kAsset; }
    QString partPath() const { return versionDir() + QStringLiteral("/.") + kAsset + QStringLiteral(".part"); }

    Config config() const {
        Config config;
        config.feedUrl = server_->url(QStringLiteral("/repos/Inverian/GitBolt/releases/latest"));
        config.repo = QStringLiteral("Inverian/GitBolt");
        config.current = Version{0, 1, 0};
        config.userAgent = kUserAgent;
        config.allowLoopback = true;
        config.downloadDir = downloadDir();
        config.requestTimeoutMs = 3000;
        return config;
    }

    // A release as the API would list it, its installer `file` and its
    // SHA256SUMS.txt (`sums`, or the right one) served by the stub.
    Release publish(const QByteArray& file, QByteArray sums = {}, const QByteArray& digest = {}) {
        if (sums.isNull())
            sums = sumsFor(file);
        server_->routes.insert(kAssetPath, Served::ok(file));
        server_->routes.insert(kSumsPath, Served::ok(sums));
        Release release;
        release.version = Version{0, 1, 1};
        release.tag = QStringLiteral("v0.1.1");
        release.pageUrl = QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1"));
        release.name = QStringLiteral("GitBolt 0.1.1");
        release.assets.push_back(
            ReleaseAsset{kSums, server_->url(QString::fromLatin1(kSumsPath)), sums.size(), sha256Hex(sums)});
        release.assets.push_back(
            ReleaseAsset{kAsset, server_->url(QString::fromLatin1(kAssetPath)), file.size(), digest});
        return release;
    }

    // Starts `release`'s installer and waits for the outcome: the
    // failure's message, or empty when it finished.
    QString run(Downloader& downloader, const Release& release, const QString& name = kAsset,
                const QString& dir = {}) {
        QSignalSpy finished(&downloader, &Downloader::finished);
        QSignalSpy failed(&downloader, &Downloader::failed);
        downloader.start(release, name, dir);
        if (!QTest::qWaitFor([&] { return finished.count() + failed.count() > 0; }, 10000))
            return QStringLiteral("(no outcome)");
        QTest::qWait(20);   // nothing else may follow
        if (finished.count() + failed.count() != 1)
            return QStringLiteral("(more than one outcome)");
        return failed.isEmpty() ? QString() : failed.at(0).at(0).toString();
    }

    // Nothing of the download is left: no installer, no .part.
    bool leftNothing() const {
        return !QFileInfo::exists(finalPath()) && !QFileInfo::exists(partPath());
    }

private slots:
    void init() {
        server_ = std::make_unique<HttpServer>();
        QVERIFY(server_->isListening());
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void cleanup() {
        server_.reset();
        dir_.reset();
    }

    // ---- success --------------------------------------------------------

    void downloadsChecksAndRenames() {
        const QByteArray file = payload(300 * 1024);
        const Release release = publish(file, {}, sha256Hex(file));
        Downloader downloader(config());
        QSignalSpy finished(&downloader, &Downloader::finished);
        QSignalSpy failed(&downloader, &Downloader::failed);
        QSignalSpy progress(&downloader, &Downloader::progress);
        bool busyInSlot = true;
        connect(&downloader, &Downloader::finished, this, [&] { busyInSlot = downloader.busy(); });

        downloader.start(release, kAsset);
        QVERIFY(downloader.busy());
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!busyInSlot);
        QVERIFY(!downloader.busy());

        QCOMPARE(finished.at(0).at(0).toString(), finalPath());
        QCOMPARE(contentsOf(finalPath()), file);
        QCOMPARE(entriesOf(versionDir()), QStringList{kAsset});   // no .part, nothing else

        QVERIFY(!progress.isEmpty());
        qint64 last = 0;
        for (const QList<QVariant>& step : std::as_const(progress)) {
            QVERIFY(step.at(0).toLongLong() > last);
            last = step.at(0).toLongLong();
            QCOMPARE(step.at(1).toLongLong(), qint64(file.size()));
        }
        QCOMPARE(last, qint64(file.size()));

        // The checksums first, then the installer, each once, each with
        // the same policy as the feed.
        QCOMPARE(server_->requests().size(), 2);
        QCOMPARE(server_->requests().at(0).path, kSumsPath);
        QCOMPARE(server_->requests().at(1).path, kAssetPath);
        for (const auto& request : server_->requests()) {
            QCOMPARE(request.method, QByteArray("GET"));
            QCOMPARE(request.header("user-agent"), kUserAgent.toLatin1());
            QCOMPARE(request.header("accept-language"), QByteArray("*"));
            QCOMPARE(request.header("accept-encoding"), QByteArray("identity"));
            QVERIFY(!request.hasHeader("cookie"));
        }
    }

    void withoutADigestTheChecksumsDecide() {
        const QByteArray file = payload(4096);
        const Release release = publish(file);
        Downloader downloader(config());
        QCOMPARE(run(downloader, release), QString());
        QCOMPARE(contentsOf(finalPath()), file);
    }

    void intoTheFolderGiven() {
        const QByteArray file = payload(10000);
        const Release release = publish(file);
        const QString folder = dir_->path() + QStringLiteral("/Applications");
        Downloader downloader(config());
        QSignalSpy finished(&downloader, &Downloader::finished);
        QCOMPARE(run(downloader, release, kAsset, folder), QString());
        QCOMPARE(finished.at(0).at(0).toString(), folder + QLatin1Char('/') + kAsset);
        QCOMPARE(contentsOf(folder + QLatin1Char('/') + kAsset), file);
        QCOMPARE(entriesOf(folder), QStringList{kAsset});
        QVERIFY(!QFileInfo::exists(downloadDir()));
    }

    void followsRedirectsToAllowedHosts() {
        const QByteArray file = payload(200 * 1024);
        Release release = publish(file);
        // github.com's answer: a redirect to the asset host, here a
        // second stub on another port (allowed: loopback).
        HttpServer assets;
        assets.routes.insert("/blob/installer?sig=1", Served::ok(file));
        assets.routes.insert("/blob/sums", Served::ok(sumsFor(file)));
        server_->routes.insert(kAssetPath, Served::redirect(assets.url(QStringLiteral("/blob/installer?sig=1"))));
        server_->routes.insert(kSumsPath, Served::redirect(assets.url(QStringLiteral("/blob/sums")), 307));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release), QString());
        QCOMPARE(contentsOf(finalPath()), file);
        QCOMPARE(server_->hits(kAssetPath), 1);
        QCOMPARE(assets.hits("/blob/installer?sig=1"), 1);
        QCOMPARE(assets.hits("/blob/sums"), 1);
        QCOMPARE(assets.requests().at(0).header("user-agent"), kUserAgent.toLatin1());
    }

    // ---- where from -------------------------------------------------------

    void aRedirectToAHostNotListedIsNeverFollowed_data() {
        QTest::addColumn<QByteArray>("redirected");
        QTest::newRow("the installer") << kAssetPath;
        QTest::newRow("the checksums") << kSumsPath;
    }
    void aRedirectToAHostNotListedIsNeverFollowed() {
        QFETCH(QByteArray, redirected);
        const QByteArray file = payload(5000);
        const Release release = publish(file);
        // "localhost" is a name, not 127.0.0.1: not on any list. The
        // stub behind it would answer, so a followed redirect shows.
        HttpServer elsewhere;
        elsewhere.routes.insert("/blob", Served::ok(file));
        const QUrl target(QStringLiteral("http://localhost:%1/blob").arg(elsewhere.port()));
        server_->routes.insert(redirected, Served::redirect(target));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("The download was sent on to http://localhost:%1, which GitBolt doesn't download "
                                "from.")
                     .arg(elsewhere.port()));
        QTest::qWait(200);
        QCOMPARE(elsewhere.requests().size(), 0);
        QVERIFY(leftNothing());
        QVERIFY(!downloader.busy());
    }

    void aFirstUrlNotListedIsNeverAsked() {
        const QByteArray file = payload(5000);
        Release release = publish(file);
        assetIn(release, kAsset)->downloadUrl =
            QUrl(QStringLiteral("http://localhost:%1%2").arg(server_->port()).arg(QString::fromLatin1(kAssetPath)));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("GitBolt doesn't download from http://localhost:%1.").arg(server_->port()));
        QCOMPARE(server_->requests().size(), 0);   // not even the checksums
        QVERIFY(leftNothing());
    }

    void plainHttpToGitHubIsRefused() {
        Release release = publish(payload(10));
        assetIn(release, kSums)->downloadUrl =
            QUrl(QStringLiteral("http://github.com/Inverian/GitBolt/releases/download/v0.1.1/SHA256SUMS.txt"));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release), QStringLiteral("GitBolt doesn't download from http://github.com."));
        QCOMPARE(server_->requests().size(), 0);
    }

    // ---- what arrives -----------------------------------------------------

    void aChecksumMismatchDeletesTheFile() {
        const QByteArray file = payload(64 * 1024);
        QByteArray sums = sumsFor(file);
        sums.replace(sha256Hex(file), sha256Hex("another file"));
        const Release release = publish(file, sums);
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("GitBolt-0.1.1-x86_64.AppImage doesn't match its checksum in SHA256SUMS.txt, so it "
                                "was deleted."));
        QCOMPARE(server_->hits(kAssetPath), 1);
        QVERIFY(leftNothing());
    }

    void aFileChangedOnTheWayIsDeleted() {
        // Both checksums describe the published file; another arrives.
        const QByteArray file = payload(64 * 1024);
        const Release release = publish(file, {}, sha256Hex(file));
        QByteArray tampered = file;
        tampered[1000] = char(tampered[1000] ^ 0x01);
        server_->routes.insert(kAssetPath, Served::ok(tampered));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("GitBolt-0.1.1-x86_64.AppImage doesn't match its checksum in SHA256SUMS.txt, so it "
                                "was deleted."));
        QVERIFY(leftNothing());
    }

    void aDigestThatDisagreesStopsItBeforeTheDownload() {
        const QByteArray file = payload(64 * 1024);
        const Release release = publish(file, {}, sha256Hex("another file"));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("GitHub's checksum for GitBolt-0.1.1-x86_64.AppImage differs from the one in "
                                "SHA256SUMS.txt."));
        QCOMPARE(server_->hits(kSumsPath), 1);
        QCOMPARE(server_->hits(kAssetPath), 0);
        QVERIFY(leftNothing());
    }

    void noChecksumLineNoDownload() {
        const QByteArray file = payload(1000);
        const QByteArray sums = QByteArray("1").repeated(64) + "  GitBolt-0.1.1-Darwin-arm64.dmg\n"
                                + sha256Hex(file) + "  GitBolt-0.1.1-x86_64.AppImage.zsync\n";
        const Release release = publish(file, sums);
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("SHA256SUMS.txt has no checksum for GitBolt-0.1.1-x86_64.AppImage."));
        QCOMPARE(server_->hits(kAssetPath), 0);
        QVERIFY(leftNothing());
    }

    void noChecksumsFileNoDownload() {
        Release release = publish(payload(1000));
        std::erase_if(release.assets, [](const ReleaseAsset& asset) { return asset.name == kSums; });
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("GitBolt 0.1.1 has no SHA256SUMS.txt, so its download can't be checked."));
        QCOMPARE(server_->requests().size(), 0);
        QVERIFY(leftNothing());
    }

    void noSuchAsset() {
        const Release release = publish(payload(1000));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release, QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg")),
                 QStringLiteral("GitBolt 0.1.1 has no GitBolt-0.1.1-Darwin-arm64.dmg."));
        QCOMPARE(server_->requests().size(), 0);
    }

    void sizeOtherThanListed_data() {
        QTest::addColumn<int>("served");
        QTest::addColumn<int>("listed");
        QTest::addColumn<QString>("message");
        QTest::newRow("shorter") << 50000 << 50001
                                 << "GitBolt-0.1.1-x86_64.AppImage arrived with 50000 bytes; GitHub lists 50001.";
        QTest::newRow("longer") << 50001 << 50000
                                << "GitBolt-0.1.1-x86_64.AppImage is larger than GitHub lists (50000 bytes).";
    }
    void sizeOtherThanListed() {
        QFETCH(int, served);
        QFETCH(int, listed);
        QFETCH(QString, message);
        const QByteArray file = payload(served);
        Release release = publish(file);   // the checksum matches what is served
        assetIn(release, kAsset)->size = listed;
        Downloader downloader(config());
        QCOMPARE(run(downloader, release), message);
        QVERIFY(leftNothing());
    }

    void checksumsOfAnotherSize() {
        const QByteArray file = payload(1000);
        Release release = publish(file);
        assetIn(release, kSums)->size += 1;
        Downloader downloader(config());
        QCOMPARE(run(downloader, release),
                 QStringLiteral("SHA256SUMS.txt arrived with %1 bytes; GitHub lists %2.")
                     .arg(sumsFor(file).size())
                     .arg(sumsFor(file).size() + 1));
        QCOMPARE(server_->hits(kAssetPath), 0);
    }

    void overTheCapIsNeverAsked() {
        const QByteArray file = payload(1024 * 1024 + 1);
        const Release release = publish(file);
        Config config = this->config();
        config.maxDownloadBytes = 1024 * 1024;
        Downloader downloader(config);
        QCOMPARE(run(downloader, release),
                 QStringLiteral("GitBolt-0.1.1-x86_64.AppImage is larger than GitBolt downloads (1 MiB)."));
        QCOMPARE(server_->requests().size(), 0);
    }

    void httpErrors_data() {
        QTest::addColumn<QByteArray>("path");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("message");
        QTest::newRow("installer 404") << kAssetPath << 404
                                       << "Couldn't download GitBolt-0.1.1-x86_64.AppImage: GitHub answered HTTP 404.";
        QTest::newRow("checksums 500") << kSumsPath << 500
                                       << "Couldn't download SHA256SUMS.txt: GitHub answered HTTP 500.";
        QTest::newRow("installer 403") << kAssetPath << 403
                                       << "Couldn't download GitBolt-0.1.1-x86_64.AppImage: GitHub answered HTTP 403.";
    }
    void httpErrors() {
        QFETCH(QByteArray, path);
        QFETCH(int, status);
        QFETCH(QString, message);
        const QByteArray file = payload(1000);
        const Release release = publish(file);
        // An error page as long as the file: its bytes must not land in
        // the .part, let alone pass for the installer.
        server_->routes.insert(path, Served::withStatus(status, file));
        Downloader downloader(config());
        QCOMPARE(run(downloader, release), message);
        QVERIFY(leftNothing());
    }

    void aStalledTransferTimesOut() {
        const QByteArray file = payload(100 * 1024);
        const Release release = publish(file);
        Served stalled = Served::ok(file);
        stalled.stallAfter = 40 * 1024;
        server_->routes.insert(kAssetPath, stalled);
        Config config = this->config();
        config.requestTimeoutMs = 300;
        Downloader downloader(config);
        const QString message = run(downloader, release);
        QVERIFY2(message.startsWith(QLatin1String("Couldn't download GitBolt-0.1.1-x86_64.AppImage: ")),
                 qPrintable(message));
        QVERIFY(leftNothing());
    }

    void aDroppedConnectionIsAFailure() {
        const QByteArray file = payload(100 * 1024);
        const Release release = publish(file);
        Served stalled = Served::ok(file);
        stalled.stallAfter = 40 * 1024;
        server_->routes.insert(kAssetPath, stalled);
        Downloader downloader(config());
        QSignalSpy failed(&downloader, &Downloader::failed);
        QSignalSpy progress(&downloader, &Downloader::progress);
        downloader.start(release, kAsset);
        QTRY_VERIFY(!progress.isEmpty() && progress.last().at(0).toLongLong() == 40 * 1024);
        server_.reset();   // closes every connection mid-body
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY2(failed.at(0).at(0).toString().startsWith(
                     QLatin1String("Couldn't download GitBolt-0.1.1-x86_64.AppImage: ")),
                 qPrintable(failed.at(0).at(0).toString()));
        QVERIFY(leftNothing());
        server_ = std::make_unique<HttpServer>();   // for cleanup()
    }

    // ---- cancel and the final name ----------------------------------------

    void cancelLeavesNothingAndSaysNothing() {
        const QByteArray file = payload(1024 * 1024);
        Release release = publish(file);
        Served slow = Served::ok(file);
        slow.chunkBytes = 16 * 1024;
        slow.chunkDelayMs = 40;
        server_->routes.insert(kAssetPath, slow);
        Downloader downloader(config());
        QSignalSpy finished(&downloader, &Downloader::finished);
        QSignalSpy failed(&downloader, &Downloader::failed);
        QSignalSpy progress(&downloader, &Downloader::progress);
        downloader.start(release, kAsset);
        QTRY_VERIFY(!progress.isEmpty());

        // Midway: the bytes so far are in the .part, the final name
        // holds nothing.
        QVERIFY(QFileInfo::exists(partPath()));
        QVERIFY(!QFileInfo::exists(finalPath()));

        downloader.cancel();
        QVERIFY(!downloader.busy());
        QVERIFY(leftNothing());
        QTRY_VERIFY(server_->disconnects() >= 1);
        QTest::qWait(200);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(failed.count(), 0);
        const qsizetype steps = progress.count();
        QTest::qWait(100);
        QCOMPARE(progress.count(), steps);

        // And it can start again.
        server_->routes.insert(kAssetPath, Served::ok(file));
        QCOMPARE(run(downloader, release), QString());
        QCOMPARE(contentsOf(finalPath()), file);
        downloader.cancel();   // idle: does nothing
        QCOMPARE(contentsOf(finalPath()), file);
    }

    void cancelBeforeAQueuedFailure() {
        Release release = publish(payload(10));
        Downloader downloader(config());
        QSignalSpy failed(&downloader, &Downloader::failed);
        downloader.start(release, QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"));   // fails, queued
        downloader.cancel();
        QTest::qWait(50);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!downloader.busy());
    }

    void theFinalNameOnlyEverHoldsACheckedFile() {
        // An earlier download at the final name (a cancelled install).
        QVERIFY(writeFile(finalPath(), "earlier"));
        const QByteArray file = payload(100 * 1024);
        Release release = publish(file);
        Served stalled = Served::ok(file);
        stalled.stallAfter = 30 * 1024;
        server_->routes.insert(kAssetPath, stalled);
        Config config = this->config();
        config.requestTimeoutMs = 500;
        Downloader downloader(config);
        QSignalSpy progress(&downloader, &Downloader::progress);
        QSignalSpy failed(&downloader, &Downloader::failed);
        downloader.start(release, kAsset);
        QTRY_VERIFY(!progress.isEmpty() && progress.last().at(0).toLongLong() == 30 * 1024);
        QCOMPARE(QFileInfo(partPath()).size(), qint64(30 * 1024));
        QCOMPARE(contentsOf(finalPath()), QByteArray("earlier"));
        QTRY_COMPARE(failed.count(), 1);   // the stall times out
        QVERIFY(!QFileInfo::exists(partPath()));
        QCOMPARE(contentsOf(finalPath()), QByteArray("earlier"));

        // A bad one doesn't replace it either; a good one does.
        QByteArray sums = sumsFor(file);
        sums.replace(sha256Hex(file), sha256Hex("x"));
        release = publish(file, sums);
        QVERIFY(!run(downloader, release).isEmpty());
        QCOMPARE(contentsOf(finalPath()), QByteArray("earlier"));
        release = publish(file);
        QCOMPARE(run(downloader, release), QString());
        QCOMPARE(contentsOf(finalPath()), file);
        QCOMPARE(entriesOf(versionDir()), QStringList{kAsset});
    }

    void aStalePartIsOverwritten() {
        QVERIFY(writeFile(partPath(), QByteArray(500 * 1024, 'z')));
        const QByteArray file = payload(1000);
        const Release release = publish(file);
        Downloader downloader(config());
        QCOMPARE(run(downloader, release), QString());
        QCOMPARE(contentsOf(finalPath()), file);
        QVERIFY(!QFileInfo::exists(partPath()));
    }

    // A link planted at the .part's name (an AppImage's folder may be
    // writable by others: a shared, sticky group folder) sends nothing
    // through it: the link goes, the .part is a new file, and the file
    // it pointed at is as it was. The final name holds a real file.
    void aLinkAtThePartsNameIsNeverFollowed() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a .lnk shortcut on Windows, which no open follows");
#else
        const QString victim = dir_->path() + QStringLiteral("/home/.ssh/authorized_keys");
        QVERIFY(writeFile(victim, "ssh-ed25519 AAAA mine\n"));
        QVERIFY(QDir().mkpath(versionDir()));
        QVERIFY(QFile::link(victim, partPath()));
        const QByteArray file = payload(20 * 1024);
        const Release release = publish(file);
        Downloader downloader(config());
        QCOMPARE(run(downloader, release), QString());
        QCOMPARE(contentsOf(victim), QByteArray("ssh-ed25519 AAAA mine\n"));
        QVERIFY(!QFileInfo(finalPath()).isSymLink());
        QCOMPARE(contentsOf(finalPath()), file);
        QCOMPARE(entriesOf(versionDir()), QStringList{kAsset});

        // A dangling link, to where a file would be created: none is.
        const QString nowhere = dir_->path() + QStringLiteral("/created-by-the-link");
        QVERIFY(QFile::remove(finalPath()));
        QVERIFY(QFile::link(nowhere, partPath()));
        QCOMPARE(run(downloader, release), QString());
        QVERIFY(!QFileInfo::exists(nowhere));
        QCOMPARE(contentsOf(finalPath()), file);
#endif
    }

    // Something at the .part's name that can't be removed (here a
    // folder with something in it): the download fails, writes nowhere
    // else, and leaves that thing alone.
    void aPartThatCantBeReplacedFailsTheDownload() {
        QVERIFY(writeFile(partPath() + QStringLiteral("/inside.txt"), "not ours"));
        const Release release = publish(payload(1000));
        Downloader downloader(config());
        const QString why = run(downloader, release);
        QVERIFY2(why.startsWith(QStringLiteral("Couldn't save %1").arg(kAsset)), qPrintable(why));
        QCOMPARE(contentsOf(partPath() + QStringLiteral("/inside.txt")), QByteArray("not ours"));
        QVERIFY(!QFileInfo::exists(finalPath()));
        QVERIFY(!downloader.busy());
    }

    // ---- calling it -------------------------------------------------------

    void startWhileBusyIsIgnored() {
        const QByteArray file = payload(256 * 1024);
        const Release release = publish(file);
        Served slow = Served::ok(file);
        slow.chunkBytes = 64 * 1024;
        slow.chunkDelayMs = 30;
        server_->routes.insert(kAssetPath, slow);
        Downloader downloader(config());
        QSignalSpy finished(&downloader, &Downloader::finished);
        QSignalSpy failed(&downloader, &Downloader::failed);
        downloader.start(release, kAsset);
        downloader.start(release, QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"));
        downloader.start(release, kAsset);
        QTRY_COMPARE(finished.count(), 1);
        QTest::qWait(100);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(server_->requests().size(), 2);
    }

    void failuresComeFromTheEventLoop() {
        const Release release = publish(payload(10));
        Downloader downloader(config());
        QSignalSpy failed(&downloader, &Downloader::failed);
        bool busyInSlot = true;
        connect(&downloader, &Downloader::failed, this, [&] { busyInSlot = downloader.busy(); });
        downloader.start(release, QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"));
        QVERIFY(downloader.busy());
        QCOMPARE(failed.count(), 0);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(!busyInSlot);
    }

    void unsafeNamesAreRefused_data() {
        QTest::addColumn<QString>("name");
        for (const char* name : {"", "../GitBolt-0.1.1-x86_64.AppImage", "sub/GitBolt.AppImage",
                                 "sub\\GitBolt.AppImage", ".hidden", "..", "a..b", "with space", "/abs"})
            QTest::newRow(*name ? name : "empty") << QString::fromLatin1(name);
    }
    void unsafeNamesAreRefused() {
        QFETCH(QString, name);
        Release release = publish(payload(10));
        assetIn(release, kAsset)->name = name;   // as if the release listed it
        Downloader downloader(config());
        QCOMPARE(run(downloader, release, name),
                 QStringLiteral("“%1” isn't a file GitBolt downloads.").arg(name));
        QCOMPARE(server_->requests().size(), 0);
        QCOMPARE(entriesOf(dir_->path()), QStringList());
    }

    void anUnusableConfigDownloadsNothing() {
        const Release release = publish(payload(10));
        Config config = this->config();
        config.downloadDir = QStringLiteral("relative/updates");
        Downloader downloader(config);
        QCOMPARE(run(downloader, release), QStringLiteral("Updates aren't set up in this copy of GitBolt."));
        QCOMPARE(server_->requests().size(), 0);
    }

    void destroyedMidwayLeavesNothing() {
        const QByteArray file = payload(512 * 1024);
        const Release release = publish(file);
        Served slow = Served::ok(file);
        slow.chunkBytes = 16 * 1024;
        slow.chunkDelayMs = 40;
        server_->routes.insert(kAssetPath, slow);
        auto downloader = std::make_unique<Downloader>(config());
        QSignalSpy progress(downloader.get(), &Downloader::progress);
        downloader->start(release, kAsset);
        QTRY_VERIFY(!progress.isEmpty());
        downloader.reset();
        QVERIFY(leftNothing());
        QTRY_VERIFY(server_->disconnects() >= 1);
    }

    // ---- removeOldDownloads -------------------------------------------------

    void removesTheRunningVersionsAndOlder() {
        const QString updates = downloadDir();
        for (const char* path : {"0.1.0/GitBolt-0.1.0-x86_64.AppImage", "0.1.1/.GitBolt-0.1.1-x86_64.AppImage.part",
                                 "0.1.1/sub/deeper", "0.1.2/GitBolt-0.1.2-x86_64.AppImage", "0.1.10/x", "v0.1.0/x",
                                 "01.1.0/x", "notes/x", "0.1/x"})
            QVERIFY(writeFile(updates + QLatin1Char('/') + QLatin1String(path), "x"));
        QVERIFY(writeFile(updates + QStringLiteral("/install.log"), "log"));
        QVERIFY(writeFile(updates + QStringLiteral("/0.0.1"), "a file named like a version"));

        removeOldDownloads(updates, Version{0, 1, 1});
        QCOMPARE(entriesOf(updates), (QStringList{QStringLiteral("0.0.1"), QStringLiteral("0.1"),
                                                  QStringLiteral("0.1.10"), QStringLiteral("0.1.2"),
                                                  QStringLiteral("01.1.0"), QStringLiteral("install.log"),
                                                  QStringLiteral("notes"), QStringLiteral("v0.1.0")}));
        QCOMPARE(entriesOf(updates + QStringLiteral("/0.1.2")),
                 QStringList{QStringLiteral("GitBolt-0.1.2-x86_64.AppImage")});

        // Nothing for a folder that isn't there, or a relative one.
        removeOldDownloads(dir_->path() + QStringLiteral("/missing"), Version{9, 9, 9});
        removeOldDownloads(QString(), Version{9, 9, 9});
        removeOldDownloads(QStringLiteral("relative"), Version{9, 9, 9});
        QCOMPARE(entriesOf(updates).size(), 8);
    }

    void aLinkIsNeverFollowed() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a .lnk shortcut on Windows, not a link the folder walk would follow");
#else
        const QString updates = downloadDir();
        const QString precious = dir_->path() + QStringLiteral("/precious");
        QVERIFY(writeFile(precious + QStringLiteral("/keep.txt"), "mine"));
        QVERIFY(QDir().mkpath(updates));
        QVERIFY(QFile::link(precious, updates + QStringLiteral("/0.1.0")));
        removeOldDownloads(updates, Version{0, 1, 1});
        QCOMPARE(contentsOf(precious + QStringLiteral("/keep.txt")), QByteArray("mine"));
        QVERIFY(QFileInfo(updates + QStringLiteral("/0.1.0")).isSymLink());
#endif
    }
};

QTEST_GUILESS_MAIN(TestUpdateDownloader)
#include "TestUpdateDownloader.moc"
