//
// TestUpdateChecker — the update check against a loopback stub
// (HttpServer.h). Never GitHub: the feed is the stub's URL, which only
// a Config with allowLoopback accepts.
//
//   - what the request carries: GET, GitHub's Accept and API version,
//     the User-Agent, Accept-Language "*", no cookies
//   - newer (0.1.1 over 0.1.0, 0.1.10 over 0.1.9) is available; the
//     same or an older release is "up to date" with the latest version
//   - GitHub's rate limit (403 with x-ratelimit-remaining: 0 or
//     retry-after, 429), other statuses, a server that never answers,
//     one that refuses connections, bad JSON, a refused release, an
//     oversized answer: each one failed() sentence
//   - a redirect is an answer, not followed
//   - one request at a time: a manual check during an automatic one
//     upgrades it; outcomes come from the event loop with busy() false
//   - a Config that isn't usable sends nothing
//

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

#include "HttpServer.h"
#include "update/Checker.h"
#include "update/Release.h"
#include "update/UpdateConfig.h"
#include "update/Version.h"

#include <memory>
#include <utility>

using namespace gitbolt::update;
using gitbolt::test::HttpServer;
using gitbolt::test::Served;

namespace {

const QByteArray kFeedPath = "/repos/Inverian/GitBolt/releases/latest";
const QString kUserAgent = QStringLiteral("GitBolt/0.1.0 (Macintosh; Mac OS X 15_6)");

QByteArray releaseJson(const QString& tag) {
    const QString version = tag.mid(1);
    QJsonObject asset{
        {QStringLiteral("name"), QStringLiteral("GitBolt-%1-x86_64.AppImage").arg(version)},
        {QStringLiteral("size"), 1234},
        {QStringLiteral("browser_download_url"),
         QStringLiteral("https://github.com/Inverian/GitBolt/releases/download/%1/GitBolt-%2-x86_64.AppImage")
             .arg(tag, version)},
    };
    QJsonObject release{
        {QStringLiteral("tag_name"), tag},
        {QStringLiteral("name"), QStringLiteral("GitBolt %1").arg(version)},
        {QStringLiteral("html_url"), QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/%1").arg(tag)},
        {QStringLiteral("draft"), false},
        {QStringLiteral("prerelease"), false},
        {QStringLiteral("body"), QStringLiteral("Notes for %1, with <i>markup</i>.").arg(tag)},
        {QStringLiteral("assets"), QJsonArray{asset}},
    };
    return QJsonDocument(release).toJson(QJsonDocument::Compact);
}

int closedPort() {
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const int port = probe.serverPort();
    probe.close();
    return port;
}

} // namespace

class TestUpdateChecker : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<HttpServer> server_;
    std::unique_ptr<QTemporaryDir> dir_;

    Config config(const Version& current = Version{0, 1, 0}) const {
        Config config;
        config.feedUrl = server_->url(QString::fromLatin1(kFeedPath));
        config.repo = QStringLiteral("Inverian/GitBolt");
        config.current = current;
        config.userAgent = kUserAgent;
        config.allowLoopback = true;
        config.downloadDir = dir_->path() + QStringLiteral("/updates");
        config.requestTimeoutMs = 3000;
        return config;
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

    void newerReleaseIsAvailable() {
        server_->routes.insert(kFeedPath, Served::json(releaseJson(QStringLiteral("v0.1.1"))));
        Checker checker(config());
        QSignalSpy available(&checker, &Checker::available);
        QSignalSpy upToDate(&checker, &Checker::upToDate);
        QSignalSpy failed(&checker, &Checker::failed);

        bool busyInSlot = true;
        connect(&checker, &Checker::available, this, [&] { busyInSlot = checker.busy(); });
        checker.check(false);
        QVERIFY(checker.busy());
        QCOMPARE(available.count(), 0);   // never from inside check()
        QTRY_COMPARE(available.count(), 1);
        QVERIFY(!busyInSlot);
        QVERIFY(!checker.busy());
        QCOMPARE(upToDate.count(), 0);
        QCOMPARE(failed.count(), 0);

        const Release release = available.at(0).at(0).value<Release>();
        QCOMPARE(release.version, (Version{0, 1, 1}));
        QCOMPARE(release.tag, QStringLiteral("v0.1.1"));
        QCOMPARE(release.notes, QStringLiteral("Notes for v0.1.1, with <i>markup</i>."));
        QVERIFY(release.asset(QStringLiteral("GitBolt-0.1.1-x86_64.AppImage")));
        QCOMPARE(available.at(0).at(1).toBool(), false);
    }

    void requestCarriesGitHubsHeadersOnly() {
        server_->routes.insert(kFeedPath, Served::json(releaseJson(QStringLiteral("v0.1.1"))));
        Checker checker(config());
        QSignalSpy available(&checker, &Checker::available);
        checker.check(true);
        QTRY_COMPARE(available.count(), 1);
        QCOMPARE(available.at(0).at(1).toBool(), true);

        QCOMPARE(server_->requests().size(), 1);
        const auto& request = server_->requests().at(0);
        QCOMPARE(request.method, QByteArray("GET"));
        QCOMPARE(request.path, kFeedPath);
        QCOMPARE(request.header("accept"), QByteArray("application/vnd.github+json"));
        QCOMPARE(request.header("x-github-api-version"), QByteArray("2022-11-28"));
        QCOMPARE(request.header("user-agent"), kUserAgent.toLatin1());
        QCOMPARE(request.header("accept-language"), QByteArray("*"));
        QCOMPARE(request.header("accept-encoding"), QByteArray("identity"));
        QVERIFY(!request.hasHeader("cookie"));
        QVERIFY(!request.hasHeader("authorization"));
    }

    void newerByNumbersNotByText() {
        server_->routes.insert(kFeedPath, Served::json(releaseJson(QStringLiteral("v0.1.10"))));
        Checker checker(config(Version{0, 1, 9}));
        QSignalSpy available(&checker, &Checker::available);
        checker.check(false);
        QTRY_COMPARE(available.count(), 1);
        QCOMPARE(available.at(0).at(0).value<Release>().version, (Version{0, 1, 10}));
    }

    void sameOrOlderIsUpToDate_data() {
        QTest::addColumn<QString>("latest");
        QTest::addColumn<QString>("current");
        QTest::newRow("same") << "v0.1.1" << "0.1.1";
        QTest::newRow("older: a development build") << "v0.1.1" << "0.1.10";
        QTest::newRow("older by a minor") << "v0.1.9" << "0.2.0";
    }
    void sameOrOlderIsUpToDate() {
        QFETCH(QString, latest);
        QFETCH(QString, current);
        server_->routes.insert(kFeedPath, Served::json(releaseJson(latest)));
        Checker checker(config(*Version::parse(current)));
        QSignalSpy upToDate(&checker, &Checker::upToDate);
        QSignalSpy available(&checker, &Checker::available);
        QSignalSpy failed(&checker, &Checker::failed);
        checker.check(true);
        QTRY_COMPARE(upToDate.count(), 1);
        QCOMPARE(upToDate.at(0).at(0).value<Version>(), *Version::parse(latest));
        QCOMPARE(upToDate.at(0).at(1).toBool(), true);
        QCOMPARE(available.count(), 0);
        QCOMPARE(failed.count(), 0);
    }

    void failures_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("header");   // "name: value", or empty
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("message");
        const QString limited = QStringLiteral("GitHub is limiting requests from your network; try again later.");
        const QByteArray limitBody = "{\"message\":\"API rate limit exceeded for 127.0.0.1.\","
                                     "\"documentation_url\":\"https://docs.github.com\"}";
        QTest::newRow("403, no requests left") << 403 << QByteArray("x-ratelimit-remaining: 0") << limitBody
                                               << limited;
        QTest::newRow("403, retry-after") << 403 << QByteArray("Retry-After: 60") << limitBody << limited;
        QTest::newRow("429") << 429 << QByteArray() << limitBody << limited;
        QTest::newRow("403, requests left: not the limit")
            << 403 << QByteArray("x-ratelimit-remaining: 12") << QByteArray("{}")
            << QStringLiteral("GitHub answered the update check with HTTP 403.");
        QTest::newRow("404") << 404 << QByteArray() << QByteArray("{\"message\":\"Not Found\"}")
                             << QStringLiteral("GitHub answered the update check with HTTP 404.");
        QTest::newRow("500") << 500 << QByteArray() << QByteArray("oops")
                             << QStringLiteral("GitHub answered the update check with HTTP 500.");
        QTest::newRow("bad JSON") << 200 << QByteArray() << QByteArray("{\"tag_name\": ")
                                  << QStringLiteral("GitHub's answer wasn't a GitBolt release: it isn't JSON.");
        QTest::newRow("HTML") << 200 << QByteArray() << QByteArray("<html>Unicorn!</html>")
                              << QStringLiteral("GitHub's answer wasn't a GitBolt release: it isn't JSON.");
        QJsonObject draft = QJsonDocument::fromJson(releaseJson(QStringLiteral("v0.1.1"))).object();
        draft.insert(QStringLiteral("draft"), true);
        QTest::newRow("a draft") << 200 << QByteArray() << QJsonDocument(draft).toJson()
                                 << QStringLiteral("GitHub's answer wasn't a GitBolt release: it is a draft.");
        QJsonObject fork = QJsonDocument::fromJson(releaseJson(QStringLiteral("v0.1.1"))).object();
        fork.insert(QStringLiteral("html_url"), QStringLiteral("https://github.com/Fork/GitBolt/releases/tag/v0.1.1"));
        QTest::newRow("another repository's release")
            << 200 << QByteArray() << QJsonDocument(fork).toJson()
            << QStringLiteral("GitHub's answer wasn't a GitBolt release: its page isn't under "
                              "https://github.com/Inverian/GitBolt/releases/.");
    }
    void failures() {
        QFETCH(int, status);
        QFETCH(QByteArray, header);
        QFETCH(QByteArray, body);
        QFETCH(QString, message);
        Served served = Served::withStatus(status, body);
        if (!header.isEmpty()) {
            const qsizetype colon = header.indexOf(':');
            served.headers.append({header.left(colon), header.mid(colon + 1).trimmed()});
        }
        server_->routes.insert(kFeedPath, served);
        Checker checker(config());
        QSignalSpy failed(&checker, &Checker::failed);
        QSignalSpy available(&checker, &Checker::available);
        QSignalSpy upToDate(&checker, &Checker::upToDate);
        checker.check(true);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), message);
        QCOMPARE(failed.at(0).at(1).toBool(), true);
        QCOMPARE(available.count() + upToDate.count(), 0);
        QVERIFY(!checker.busy());
    }

    void aServerThatNeverAnswersTimesOut() {
        server_->routes.insert(kFeedPath, Served::never());
        Config config = this->config();
        config.requestTimeoutMs = 300;
        Checker checker(config);
        QSignalSpy failed(&checker, &Checker::failed);
        checker.check(false);
        QTRY_COMPARE(failed.count(), 1);
        const QString message = failed.at(0).at(0).toString();
        QVERIFY2(message.startsWith(QLatin1String("Couldn't reach GitHub: ")), qPrintable(message));
        QVERIFY2(message.endsWith(u'.'), qPrintable(message));
        QCOMPARE(failed.at(0).at(1).toBool(), false);
        QCOMPARE(server_->hits(kFeedPath), 1);
    }

    void anAnswerCutOffMidwayIsAFailure() {
        Served served = Served::json(releaseJson(QStringLiteral("v0.1.1")));
        served.stallAfter = 20;
        server_->routes.insert(kFeedPath, served);
        Config config = this->config();
        config.requestTimeoutMs = 300;
        Checker checker(config);
        QSignalSpy failed(&checker, &Checker::failed);
        checker.check(true);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY2(failed.at(0).at(0).toString().startsWith(QLatin1String("Couldn't reach GitHub: ")),
                 qPrintable(failed.at(0).at(0).toString()));
    }

    void refusedConnection() {
        Config config = this->config();
        config.feedUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/latest").arg(closedPort()));
        Checker checker(config);
        QSignalSpy failed(&checker, &Checker::failed);
        checker.check(true);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("Couldn't reach GitHub: Connection refused."));
    }

    void redirectsAreNotFollowed() {
        HttpServer elsewhere;
        elsewhere.routes.insert("/latest", Served::json(releaseJson(QStringLiteral("v0.1.1"))));
        server_->routes.insert(kFeedPath, Served::redirect(elsewhere.url(QStringLiteral("/latest"))));
        Checker checker(config());
        QSignalSpy failed(&checker, &Checker::failed);
        QSignalSpy available(&checker, &Checker::available);
        checker.check(true);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("GitHub answered the update check with HTTP 302."));
        QTest::qWait(100);
        QCOMPARE(elsewhere.requests().size(), 0);
        QCOMPARE(available.count(), 0);
    }

    void anOversizedAnswerIsCutOff() {
        QByteArray huge = releaseJson(QStringLiteral("v0.1.1"));
        huge.chop(1);
        huge += ",\"padding\":\"" + QByteArray(5 * 1024 * 1024, 'x') + "\"}";
        server_->routes.insert(kFeedPath, Served::json(huge));
        Checker checker(config());
        QSignalSpy failed(&checker, &Checker::failed);
        checker.check(true);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("GitHub's answer wasn't a GitBolt release: it is too large."));
    }

    void aManualCheckUpgradesTheRunningOne() {
        Served slow = Served::json(releaseJson(QStringLiteral("v0.1.1")));
        slow.delayMs = 300;
        server_->routes.insert(kFeedPath, slow);
        Checker checker(config());
        QSignalSpy available(&checker, &Checker::available);
        checker.check(false);
        QTRY_COMPARE(server_->requests().size(), 1);
        checker.check(true);    // upgrades
        checker.check(false);   // never downgrades
        QTRY_COMPARE(available.count(), 1);
        QCOMPARE(available.at(0).at(1).toBool(), true);
        QTest::qWait(100);
        QCOMPARE(server_->requests().size(), 1);   // one request for all three
        QCOMPARE(available.count(), 1);

        // The flag belonged to that check: the next one starts afresh.
        server_->routes.insert(kFeedPath, Served::json(releaseJson(QStringLiteral("v0.1.1"))));
        checker.check(false);
        QTRY_COMPARE(available.count(), 2);
        QCOMPARE(available.at(1).at(1).toBool(), false);
        QCOMPARE(server_->requests().size(), 2);
    }

    void aSlotMayCheckAgain() {
        server_->routes.insert(kFeedPath, Served::json(releaseJson(QStringLiteral("v0.1.0"))));
        Checker checker(config());
        QSignalSpy upToDate(&checker, &Checker::upToDate);
        bool again = true;
        connect(&checker, &Checker::upToDate, this, [&] {
            if (std::exchange(again, false))
                checker.check(true);
        });
        checker.check(false);
        QTRY_COMPARE(upToDate.count(), 2);
        QCOMPARE(upToDate.at(1).at(1).toBool(), true);
        QCOMPARE(server_->requests().size(), 2);
    }

    void anUnusableConfigSendsNothing() {
        Config config = this->config();
        config.userAgent.clear();
        Checker checker(config);
        QSignalSpy failed(&checker, &Checker::failed);
        checker.check(true);
        QVERIFY(checker.busy());
        QCOMPARE(failed.count(), 0);   // queued, like every outcome
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("Updates aren't set up in this copy of GitBolt."));
        QCOMPARE(failed.at(0).at(1).toBool(), true);
        QVERIFY(!checker.busy());
        QTest::qWait(50);
        QCOMPARE(server_->requests().size(), 0);
    }

    void aFeedThatIsNotAllowedSendsNothing() {
        Config config = this->config();
        config.allowLoopback = false;   // the stub's http://127.0.0.1 is then refused
        Checker checker(config);
        QSignalSpy failed(&checker, &Checker::failed);
        checker.check(false);
        QTRY_COMPARE(failed.count(), 1);
        QTest::qWait(50);
        QCOMPARE(server_->requests().size(), 0);
    }

    void destroyedWhileBusyReportsNothing() {
        server_->routes.insert(kFeedPath, Served::never());
        auto checker = std::make_unique<Checker>(config());
        int signals_ = 0;
        connect(checker.get(), &Checker::failed, this, [&] { ++signals_; });
        connect(checker.get(), &Checker::available, this, [&] { ++signals_; });
        connect(checker.get(), &Checker::upToDate, this, [&] { ++signals_; });
        checker->check(true);
        QTRY_COMPARE(server_->requests().size(), 1);
        checker.reset();
        QTRY_COMPARE(server_->disconnects(), 1);
        QCOMPARE(signals_, 0);
    }
};

QTEST_GUILESS_MAIN(TestUpdateChecker)
#include "TestUpdateChecker.moc"
