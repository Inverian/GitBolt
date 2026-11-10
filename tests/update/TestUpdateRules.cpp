//
// TestUpdateRules — the in-app updater's rules, without a network:
//
//   - Version: what parses (x.y.z, an optional "v") and what doesn't,
//     and that 0.1.10 is newer than 0.1.9 (a string compare says not)
//   - the Config's URL rules: the feed only at api.github.com, downloads
//     only from GitHub's hosts, https only, plain http to loopback only
//     with allowLoopback; and what makes a Config usable
//   - dueForAutoCheck (24 h, a clock set back) and the
//     GITBOLT_UPDATE_CHECK opt-out
//   - parseRelease on GitHub-shaped fixtures (fixtures/update, 0.1.x):
//     the fields kept, GitHub's text kept as written, and every refusal
//     (draft, pre-release, a bad tag, a foreign page, ...)
//   - checksumFor on sha256sum's output
//   - the installer of each package is picked by name, never the
//     release's -symbols.zip files beside it
//   - systemTooOld, the minimum-system comparison macOS's and Windows'
//     checks share
//

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>

#include "update/Installer.h"
#include "update/Package.h"
#include "update/Release.h"
#include "update/UpdateConfig.h"
#include "update/Version.h"

#include <optional>

using namespace gitbolt::update;

namespace {

const QString kRepo = QStringLiteral("Inverian/GitBolt");

QByteArray fixture(const char* name) {
    QFile file(QStringLiteral(GITBOLT_UPDATE_FIXTURES "/") + QLatin1String(name));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", name);
    return file.readAll();
}

QJsonObject fixtureObject(const char* name) {
    return QJsonDocument::fromJson(fixture(name)).object();
}

QByteArray toJson(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

Config goodConfig() {
    Config config;
    config.feedUrl = QUrl(QStringLiteral("http://127.0.0.1:8123/repos/Inverian/GitBolt/releases/latest"));
    config.allowLoopback = true;
    config.repo = kRepo;
    config.current = Version{0, 1, 0};
    config.userAgent = QStringLiteral("GitBolt/0.1.0 (Macintosh; Mac OS X 15_6)");
    config.downloadDir = QDir::tempPath() + QStringLiteral("/gitbolt-update-rules");
    return config;
}

const QString kHex = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

} // namespace

class TestUpdateRules : public QObject {
    Q_OBJECT

private slots:
    // ---- Version --------------------------------------------------------

    void versionParses_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("majorPart");
        QTest::addColumn<int>("minorPart");
        QTest::addColumn<int>("patchPart");
        QTest::newRow("plain") << "0.1.0" << 0 << 1 << 0;
        QTest::newRow("with v") << "v0.1.10" << 0 << 1 << 10;
        QTest::newRow("all zero") << "0.0.0" << 0 << 0 << 0;
        QTest::newRow("six digits a part") << "999999.123456.1" << 999999 << 123456 << 1;
    }
    void versionParses() {
        QFETCH(QString, text);
        QFETCH(int, majorPart);
        QFETCH(int, minorPart);
        QFETCH(int, patchPart);
        const std::optional<Version> version = Version::parse(text);
        QVERIFY(version);
        QCOMPARE(version->majorPart, majorPart);
        QCOMPARE(version->minorPart, minorPart);
        QCOMPARE(version->patchPart, patchPart);
        QCOMPARE(version->toString(), text.startsWith(u'v') ? text.mid(1) : text);
    }

    void versionRefuses_data() {
        QTest::addColumn<QString>("text");
        for (const char* text : {"", "v", "0", "0.1", "0.1.", ".1.0", "0..1", "0.1.0.1", "0.1.0-rc1", "0.1.0+build",
                                 " 0.1.0", "0.1.0 ", "V0.1.0", "vv0.1.0", "01.1.0", "0.01.0", "0.1.00", "1234567.0.0",
                                 "0.1.a", "-1.0.0", "0,1,0"})
            QTest::newRow(*text ? text : "empty") << QString::fromLatin1(text);
        QTest::newRow("a newline") << QStringLiteral("0.1.0\n");
        // Arabic-Indic digits are digits to QChar, not to a version.
        QTest::newRow("non-ASCII digits") << QStringLiteral("٠.١.٠");
    }
    void versionRefuses() {
        QFETCH(QString, text);
        QVERIFY(!Version::parse(text));
    }

    void versionOrdersPartByPartAsNumbers() {
        const auto v = [](const char* text) { return *Version::parse(QString::fromLatin1(text)); };
        QVERIFY(v("0.1.9") < v("0.1.10"));
        QVERIFY(v("0.1.10") < v("0.2.0"));
        QVERIFY(v("0.2.0") < v("1.0.0"));
        QVERIFY(v("0.1.1") > v("0.1.0"));
        QVERIFY(v("v0.1.1") == v("0.1.1"));
        QVERIFY(v("0.1.1") >= v("0.1.1"));
        QVERIFY(!(v("0.1.10") < v("0.1.9")));
    }

    // ---- Config: URL rules --------------------------------------------------

    void feedUrls_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("allowLoopback");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("official") << QString::fromLatin1(kOfficialFeed) << false << true;
        QTest::newRow("official, loopback on too") << QString::fromLatin1(kOfficialFeed) << true << true;
        QTest::newRow("explicit 443") << "https://api.github.com:443/repos/Inverian/GitBolt/releases/latest" << false
                                      << true;
        QTest::newRow("http") << "http://api.github.com/repos/Inverian/GitBolt/releases/latest" << false << false;
        QTest::newRow("other port") << "https://api.github.com:8443/repos/x" << false << false;
        QTest::newRow("user info") << "https://me@api.github.com/repos/x" << false << false;
        QTest::newRow("another host") << "https://releases.example.com/latest" << false << false;
        QTest::newRow("a lookalike") << "https://api.github.com.example.com/latest" << false << false;
        QTest::newRow("trailing dot") << "https://api.github.com./latest" << false << false;
        QTest::newRow("github.com") << "https://github.com/Inverian/GitBolt/releases/latest" << false << false;
        QTest::newRow("loopback v4, off") << "http://127.0.0.1:8123/latest" << false << false;
        QTest::newRow("loopback v4") << "http://127.0.0.1:8123/latest" << true << true;
        QTest::newRow("loopback v6") << "http://[::1]:8123/latest" << true << true;
        QTest::newRow("loopback, default port") << "http://127.0.0.1/latest" << true << true;
        QTest::newRow("localhost is a name") << "http://localhost:8123/latest" << true << false;
        QTest::newRow("another 127/8 address") << "http://127.0.0.2:8123/latest" << true << false;
        QTest::newRow("https loopback") << "https://127.0.0.1:8123/latest" << true << false;
        QTest::newRow("loopback with user info") << "http://me@127.0.0.1:8123/latest" << true << false;
        QTest::newRow("file") << "file:///tmp/latest.json" << true << false;
        QTest::newRow("empty") << "" << true << false;
    }
    void feedUrls() {
        QFETCH(QString, url);
        QFETCH(bool, allowLoopback);
        QFETCH(bool, allowed);
        Config config;
        config.allowLoopback = allowLoopback;
        QCOMPARE(config.isAllowedFeedUrl(QUrl(url)), allowed);
    }

    void downloadUrls_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("allowLoopback");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("github.com")
            << "https://github.com/Inverian/GitBolt/releases/download/v0.1.1/GitBolt-0.1.1-x86_64.AppImage" << false
            << true;
        QTest::newRow("objects") << "https://objects.githubusercontent.com/github-production-release-asset/1?x=y"
                                 << false << true;
        QTest::newRow("release-assets") << "https://release-assets.githubusercontent.com/github-production/1" << false
                                        << true;
        QTest::newRow("upper case host") << "https://GitHub.com/Inverian/GitBolt/releases/download/v0.1.1/x" << false
                                         << true;
        QTest::newRow("http") << "http://github.com/Inverian/GitBolt/releases/download/v0.1.1/x" << false << false;
        QTest::newRow("http asset host") << "http://objects.githubusercontent.com/x" << false << false;
        QTest::newRow("other port") << "https://github.com:444/x" << false << false;
        QTest::newRow("user info") << "https://me:secret@github.com/x" << false << false;
        QTest::newRow("a lookalike") << "https://github.com.example.com/x" << false << false;
        QTest::newRow("a suffix") << "https://evilgithub.com/x" << false << false;
        QTest::newRow("a subdomain not listed") << "https://raw.githubusercontent.com/x" << false << false;
        QTest::newRow("trailing dot") << "https://github.com./x" << false << false;
        QTest::newRow("the API") << "https://api.github.com/repos/x/releases/assets/1" << false << false;
        QTest::newRow("ftp") << "ftp://github.com/x" << false << false;
        QTest::newRow("loopback, off") << "http://127.0.0.1:9000/x" << false << false;
        QTest::newRow("loopback") << "http://127.0.0.1:9000/x" << true << true;
        QTest::newRow("loopback v6") << "http://[::1]:9000/x" << true << true;
        QTest::newRow("localhost is a name") << "http://localhost:9000/x" << true << false;
        QTest::newRow("github.com, loopback on") << "https://github.com/x" << true << true;
        QTest::newRow("http github.com, loopback on") << "http://github.com/x" << true << false;
    }
    void downloadUrls() {
        QFETCH(QString, url);
        QFETCH(bool, allowLoopback);
        QFETCH(bool, allowed);
        Config config;
        config.allowLoopback = allowLoopback;
        QCOMPARE(config.isAllowedDownloadUrl(QUrl(url)), allowed);
    }

    void downloadHostsComeFromTheConfig() {
        Config config;
        config.downloadHosts = {QStringLiteral("assets.example.org")};
        QVERIFY(config.isAllowedDownloadUrl(QUrl(QStringLiteral("https://assets.example.org/x"))));
        QVERIFY(!config.isAllowedDownloadUrl(QUrl(QStringLiteral("https://github.com/x"))));
    }

    void loopbackHttpUrls() {
        QVERIFY(isLoopbackHttpUrl(QUrl(QStringLiteral("http://127.0.0.1:1/x"))));
        QVERIFY(isLoopbackHttpUrl(QUrl(QStringLiteral("http://[::1]:65535/"))));
        QVERIFY(!isLoopbackHttpUrl(QUrl(QStringLiteral("https://127.0.0.1:1/x"))));
        QVERIFY(!isLoopbackHttpUrl(QUrl(QStringLiteral("http://localhost:1/x"))));
        QVERIFY(!isLoopbackHttpUrl(QUrl(QStringLiteral("http://u@127.0.0.1:1/x"))));
        QVERIFY(!isLoopbackHttpUrl(QUrl(QString::fromLatin1(kOfficialFeed))));
    }

    void officialConstants() {
        QCOMPARE(QString::fromLatin1(kOfficialRepo), kRepo);
        QCOMPARE(kAutoCheckHours, 24);
        Config config = goodConfig();
        config.feedUrl = QUrl(QString::fromLatin1(kOfficialFeed));
        config.allowLoopback = false;
        config.repo = QString::fromLatin1(kOfficialRepo);
        QVERIFY(config.isUsable());
    }

    // ---- Config: usable -----------------------------------------------------

    void usableConfig_data() {
        QTest::addColumn<QString>("broken");
        QTest::newRow("as filled") << QString();
        for (const char* field : {"feed", "feed needs loopback", "repo empty", "repo without owner", "repo with dots",
                                  "repo with a query", "version 0.0.0", "no user agent", "user agent newline",
                                  "no download dir", "relative download dir", "no timeout", "no size cap"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void usableConfig() {
        QFETCH(QString, broken);
        Config config = goodConfig();
        if (broken == QLatin1String("feed"))
            config.feedUrl = QUrl(QStringLiteral("https://example.com/latest"));
        else if (broken == QLatin1String("feed needs loopback"))
            config.allowLoopback = false;
        else if (broken == QLatin1String("repo empty"))
            config.repo.clear();
        else if (broken == QLatin1String("repo without owner"))
            config.repo = QStringLiteral("GitBolt");
        else if (broken == QLatin1String("repo with dots"))
            config.repo = QStringLiteral("Inverian/..");
        else if (broken == QLatin1String("repo with a query"))
            config.repo = QStringLiteral("Inverian/GitBolt?x=1");
        else if (broken == QLatin1String("version 0.0.0"))
            config.current = Version{};
        else if (broken == QLatin1String("no user agent"))
            config.userAgent.clear();
        else if (broken == QLatin1String("user agent newline"))
            config.userAgent = QStringLiteral("GitBolt/0.1.0\r\nX-Evil: 1");
        else if (broken == QLatin1String("no download dir"))
            config.downloadDir.clear();
        else if (broken == QLatin1String("relative download dir"))
            config.downloadDir = QStringLiteral("cache/updates");
        else if (broken == QLatin1String("no timeout"))
            config.requestTimeoutMs = 0;
        else if (broken == QLatin1String("no size cap"))
            config.maxDownloadBytes = 0;
        QCOMPARE(config.isUsable(), broken.isEmpty());
    }

    void defaults() {
        const Config config;
        QCOMPARE(config.downloadHosts, (QStringList{QStringLiteral("github.com"),
                                                    QStringLiteral("objects.githubusercontent.com"),
                                                    QStringLiteral("release-assets.githubusercontent.com")}));
        QVERIFY(!config.allowLoopback);
        QCOMPARE(config.requestTimeoutMs, 15000);
        QCOMPARE(config.maxDownloadBytes, 512LL * 1024 * 1024);
        QVERIFY(!config.isUsable());
    }

    // ---- the schedule and the opt-out -------------------------------------

    void dueForAutoCheck_data() {
        QTest::addColumn<qint64>("lastSecsAgo");   // negative: in the future
        QTest::addColumn<int>("hours");
        QTest::addColumn<bool>("due");
        QTest::newRow("just checked") << qint64(0) << 24 << false;
        QTest::newRow("23:59:59 ago") << qint64(24 * 3600 - 1) << 24 << false;
        QTest::newRow("24 h ago") << qint64(24 * 3600) << 24 << true;
        QTest::newRow("a week ago") << qint64(7 * 24 * 3600) << 24 << true;
        QTest::newRow("30 min ahead: clock skew") << qint64(-1800) << 24 << false;
        QTest::newRow("1 h ahead") << qint64(-3600) << 24 << false;
        QTest::newRow("1 h 1 s ahead: clock set back") << qint64(-3601) << 24 << true;
        QTest::newRow("a year ahead") << qint64(-365LL * 24 * 3600) << 24 << true;
        QTest::newRow("1 h rule, 59 min") << qint64(59 * 60) << 1 << false;
        QTest::newRow("1 h rule, 1 h") << qint64(3600) << 1 << true;
    }
    void dueForAutoCheck() {
        QFETCH(qint64, lastSecsAgo);
        QFETCH(int, hours);
        QFETCH(bool, due);
        const QDateTime now(QDate(2026, 11, 7), QTime(12, 0), QTimeZone::UTC);
        QCOMPARE(gitbolt::update::dueForAutoCheck(now.addSecs(-lastSecsAgo), now, hours), due);
    }

    void dueWhenNeverChecked() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(gitbolt::update::dueForAutoCheck(QDateTime(), now));
        // As main() reads it back: UTC ISO 8601, or garbage.
        QVERIFY(gitbolt::update::dueForAutoCheck(QDateTime::fromString(QStringLiteral("yesterday"), Qt::ISODate),
                                                 now));
        const QDateTime saved = QDateTime::fromString(now.addSecs(-60).toString(Qt::ISODate), Qt::ISODate);
        QVERIFY(saved.isValid());
        QVERIFY(!gitbolt::update::dueForAutoCheck(saved, now));
        QCOMPARE(gitbolt::update::dueForAutoCheck(saved, now, kAutoCheckHours),
                 gitbolt::update::dueForAutoCheck(saved, now));
        // Saved in UTC, compared with a local clock: the same instant.
        QVERIFY(!gitbolt::update::dueForAutoCheck(saved, now.toLocalTime()));
        QVERIFY(gitbolt::update::dueForAutoCheck(saved, QDateTime()));
    }

    void optOut_data() {
        QTest::addColumn<QString>("value");   // null: unset
        QTest::addColumn<bool>("optedOut");
        QTest::newRow("unset") << QString() << false;
        QTest::newRow("empty") << QStringLiteral("") << false;
        for (const char* off : {"0", "false", "FALSE", "Off", "no", " no ", "\tOFF\n"})
            QTest::newRow(off) << QString::fromLatin1(off) << true;
        for (const char* on : {"1", "true", "on", "yes", "nope", "00", "f"})
            QTest::newRow(on) << QString::fromLatin1(on) << false;
    }
    void optOut() {
        QFETCH(QString, value);
        QFETCH(bool, optedOut);
        QProcessEnvironment env;
        env.insert(QStringLiteral("GITBOLT_TELEMETRY"), QStringLiteral("0"));   // a different switch
        if (!value.isNull())
            env.insert(QStringLiteral("GITBOLT_UPDATE_CHECK"), value);
        QCOMPARE(updateCheckOptedOut(env), optedOut);
    }

    // ---- parseRelease -----------------------------------------------------

    void parsesARelease() {
        const ParsedRelease parsed = parseRelease(fixture("release-v0.1.1.json"), kRepo);
        QVERIFY2(parsed.release, qPrintable(parsed.error));
        QVERIFY(parsed.error.isEmpty());
        const Release& release = *parsed.release;
        QCOMPARE(release.version, (Version{0, 1, 1}));
        QCOMPARE(release.tag, QStringLiteral("v0.1.1"));
        QCOMPARE(release.pageUrl, QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1")));
        QCOMPARE(release.name, QStringLiteral("GitBolt 0.1.1"));
        // GitHub's text as written: markup and links untouched, for a
        // dialog that shows it as plain text.
        QVERIFY(release.notes.contains(QLatin1String("<b>markup</b>")));
        QVERIFY(release.notes.contains(QLatin1String("[link](https://example.com/notes)")));
        QVERIFY(release.notes.startsWith(QLatin1String("A made-up release")));

        QCOMPARE(release.assets.size(), size_t(5));
        const ReleaseAsset* dmg = release.asset(QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"));
        QVERIFY(dmg);
        QCOMPARE(dmg->size, qint64(41234567));
        QCOMPARE(dmg->downloadUrl,
                 QUrl(QStringLiteral(
                     "https://github.com/Inverian/GitBolt/releases/download/v0.1.1/GitBolt-0.1.1-Darwin-arm64.dmg")));
        QCOMPARE(dmg->sha256, QByteArray("ab").repeated(32));   // lower-cased
        const ReleaseAsset* exe = release.asset(QStringLiteral("GitBolt-0.1.1-Windows-AMD64.exe"));
        QVERIFY(exe);
        QCOMPARE(exe->sha256, QByteArray("0123456789abcdef").repeated(4));
        const ReleaseAsset* appImage = release.asset(QStringLiteral("GitBolt-0.1.1-x86_64.AppImage"));
        QVERIFY(appImage);
        QVERIFY(appImage->sha256.isEmpty());   // no digest in the answer
        QVERIFY(release.asset(QString::fromLatin1(kChecksumsAsset)));
        QCOMPARE(release.asset(QString::fromLatin1(kChecksumsAsset))->size, qint64(385));
        QVERIFY(!release.asset(QStringLiteral("gitbolt_0.1.1_amd64.DEB")));   // exact names only
        QVERIFY(!release.asset(QString()));
    }

    void missingAssetIsJustMissing() {
        const ParsedRelease parsed = parseRelease(fixture("missing-asset-v0.1.2.json"), kRepo);
        QVERIFY2(parsed.release, qPrintable(parsed.error));
        const Release& release = *parsed.release;
        QCOMPARE(release.version, (Version{0, 1, 2}));
        QVERIFY(!release.asset(QStringLiteral("GitBolt-0.1.2-Windows-AMD64.exe")));
        QVERIFY(release.asset(QStringLiteral("GitBolt-0.1.2-Darwin-arm64.dmg")));
        // The entry without a size is skipped, not fatal.
        QVERIFY(!release.asset(QStringLiteral("GitBolt-0.1.2-notes.txt")));
        QCOMPARE(release.assets.size(), size_t(4));
        // No title: the tag stands in. No body: no notes.
        QCOMPARE(release.name, QStringLiteral("v0.1.2"));
        QVERIFY(release.notes.isEmpty());
    }

    void refusesFixtures_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("error");
        QTest::newRow("draft") << "draft-v0.1.1.json" << "it is a draft";
        QTest::newRow("pre-release") << "prerelease-v0.1.1.json" << "it is a pre-release";
        QTest::newRow("bad tag") << "bad-tag-v0.1.1-rc1.json"
                                 << QStringLiteral("its tag “v0.1.1-rc1” isn't v<x.y.z>");
        QTest::newRow("foreign page") << "foreign-page-v0.1.1.json"
                                      << "its page isn't under https://github.com/Inverian/GitBolt/releases/";
    }
    void refusesFixtures() {
        QFETCH(QString, file);
        QFETCH(QString, error);
        const ParsedRelease parsed = parseRelease(fixture(qPrintable(file)), kRepo);
        QVERIFY(!parsed.release);
        QCOMPARE(parsed.error, error);
    }

    // The same good fixture, one field broken at a time.
    void refusesVariants_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");   // Undefined: removed
        QTest::addColumn<QString>("error");
        const QJsonValue gone(QJsonValue::Undefined);
        QTest::newRow("no draft flag") << "draft" << gone << "it doesn't say whether it is a draft";
        QTest::newRow("draft flag a string") << "draft" << QJsonValue("false")
                                             << "it doesn't say whether it is a draft";
        QTest::newRow("no pre-release flag") << "prerelease" << gone << "it doesn't say whether it is a pre-release";
        QTest::newRow("no tag") << "tag_name" << gone << "it has no tag";
        QTest::newRow("tag without v") << "tag_name" << QJsonValue("0.1.1")
                                       << QStringLiteral("its tag “0.1.1” isn't v<x.y.z>");
        QTest::newRow("two-part tag") << "tag_name" << QJsonValue("v0.1")
                                      << QStringLiteral("its tag “v0.1” isn't v<x.y.z>");
        QTest::newRow("long tag, shortened") << "tag_name" << QJsonValue(QString(QStringLiteral("v0.1.1\n") +
                                                                                   QString(100, u'x')))
                                             << QStringLiteral("its tag “v0.1.1 %1…” isn't v<x.y.z>")
                                                    .arg(QString(33, u'x'));
        const QString under = QStringLiteral("its page isn't under https://github.com/Inverian/GitBolt/releases/");
        QTest::newRow("no page") << "html_url" << gone << under;
        QTest::newRow("http page") << "html_url" << QJsonValue("http://github.com/Inverian/GitBolt/releases/tag/v0.1.1")
                                   << under;
        QTest::newRow("page on another host")
            << "html_url" << QJsonValue("https://github.com.example.com/Inverian/GitBolt/releases/tag/v0.1.1") << under;
        QTest::newRow("page climbing out")
            << "html_url" << QJsonValue("https://github.com/Inverian/GitBolt/releases/../../../Other/x") << under;
        QTest::newRow("page with a query")
            << "html_url" << QJsonValue("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1?x=@evil") << under;
        QTest::newRow("page encoded")
            << "html_url" << QJsonValue("https://github.com/Inverian/GitBolt/releases/%2e%2e/%2e%2e/x") << under;
        QTest::newRow("page of a same-prefix repo")
            << "html_url" << QJsonValue("https://github.com/Inverian/GitBolt-Server/releases/tag/v0.1.1") << under;
    }
    void refusesVariants() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QFETCH(QString, error);
        QJsonObject object = fixtureObject("release-v0.1.1.json");
        if (value.isUndefined())
            object.remove(field);
        else
            object.insert(field, value);
        const ParsedRelease parsed = parseRelease(toJson(object), kRepo);
        QVERIFY(!parsed.release);
        QCOMPARE(parsed.error, error);
    }

    void refusesWhatIsNotAReleaseObject() {
        QCOMPARE(parseRelease("{not json", kRepo).error, QStringLiteral("it isn't JSON"));
        QCOMPARE(parseRelease("", kRepo).error, QStringLiteral("it isn't JSON"));
        QCOMPARE(parseRelease("[]", kRepo).error, QStringLiteral("it isn't a release object"));
        // GitHub's own error answer, should a 200 ever carry one.
        const QByteArray notFound = "{\"message\":\"Not Found\",\"documentation_url\":\"https://docs.github.com\"}";
        QCOMPARE(parseRelease(notFound, kRepo).error, QStringLiteral("it doesn't say whether it is a draft"));
    }

    void refusesWhenTheRepoDiffers() {
        const QByteArray json = fixture("release-v0.1.1.json");
        QVERIFY(parseRelease(json, kRepo).release);
        QVERIFY(!parseRelease(json, QStringLiteral("SomeoneElse/GitBolt")).release);
        QVERIFY(!parseRelease(json, QStringLiteral("Inverian")).release);
        QVERIFY(!parseRelease(json, QString()).release);
        // The page of a fork names the fork: accepted for the fork only.
        QVERIFY(parseRelease(fixture("foreign-page-v0.1.1.json"), QStringLiteral("SomeoneElse/GitBolt")).release);
    }

    void refusesAnAssetListedTwice() {
        QJsonObject object = fixtureObject("release-v0.1.1.json");
        QJsonArray assets = object.value(QLatin1String("assets")).toArray();
        QJsonObject copy = assets.at(0).toObject();
        copy.insert(QStringLiteral("browser_download_url"), QStringLiteral("https://github.com/x/y"));
        assets.append(copy);
        object.insert(QStringLiteral("assets"), assets);
        const ParsedRelease parsed = parseRelease(toJson(object), kRepo);
        QVERIFY(!parsed.release);
        QCOMPARE(parsed.error, QStringLiteral("it lists “GitBolt-0.1.1-Darwin-arm64.dmg” twice"));
    }

    void skipsMalformedAssetEntries() {
        QJsonObject object = fixtureObject("release-v0.1.1.json");
        QJsonArray assets;
        auto entry = [](const QJsonObject& from, const char* key, const QJsonValue& value) {
            QJsonObject changed = from;
            if (value.isUndefined())
                changed.remove(QLatin1String(key));
            else
                changed.insert(QLatin1String(key), value);
            return changed;
        };
        const QJsonObject good = object.value(QLatin1String("assets")).toArray().at(0).toObject();
        assets.append(QJsonValue("not an object"));
        assets.append(entry(good, "name", QJsonValue("")));
        assets.append(entry(entry(good, "name", QJsonValue("a")), "browser_download_url", QJsonValue("")));
        assets.append(entry(entry(good, "name", QJsonValue("b")), "browser_download_url", QJsonValue("/relative")));
        assets.append(entry(entry(good, "name", QJsonValue("c")), "size", QJsonValue(-1)));
        assets.append(entry(entry(good, "name", QJsonValue("d")), "size", QJsonValue(1.5)));
        assets.append(entry(entry(good, "name", QJsonValue("e")), "size", QJsonValue("12")));
        assets.append(entry(entry(good, "name", QJsonValue("f")), "digest", QJsonValue("sha512:abc")));
        assets.append(entry(entry(good, "name", QJsonValue("g")), "digest", QJsonValue("sha256:not-hex")));
        object.insert(QStringLiteral("assets"), assets);
        const ParsedRelease parsed = parseRelease(toJson(object), kRepo);
        QVERIFY2(parsed.release, qPrintable(parsed.error));
        QCOMPARE(parsed.release->assets.size(), size_t(2));
        QVERIFY(parsed.release->asset(QStringLiteral("f"))->sha256.isEmpty());   // another algorithm
        // A malformed sha256 is kept, so it can never match a download.
        QCOMPARE(parsed.release->asset(QStringLiteral("g"))->sha256, QByteArray("not-hex"));
    }

    // 1.0.0's releases carry symbol files beside the installers
    // (GitBolt-<v>-<platform>-symbols.zip): each package still installs
    // from its own installer, never a zip.
    void symbolFilesAreNeverPicked() {
        QJsonObject object = fixtureObject("release-v0.1.1.json");
        QJsonArray assets = object.value(QStringLiteral("assets")).toArray();
        const qsizetype before = assets.size();
        const QStringList zips{QStringLiteral("GitBolt-0.1.1-Darwin-arm64-symbols.zip"),
                               QStringLiteral("GitBolt-0.1.1-Windows-AMD64-symbols.zip"),
                               QStringLiteral("GitBolt-0.1.1-Linux-x86_64-symbols.zip")};
        for (const QString& zip : zips) {
            assets.append(QJsonObject{
                {QStringLiteral("name"), zip},
                {QStringLiteral("size"), 1000},
                {QStringLiteral("browser_download_url"),
                 QStringLiteral("https://github.com/Inverian/GitBolt/releases/download/v0.1.1/") + zip},
            });
        }
        object.insert(QStringLiteral("assets"), assets);
        const ParsedRelease parsed = parseRelease(toJson(object), kRepo);
        QVERIFY2(parsed.release, qPrintable(parsed.error));
        QCOMPARE(qsizetype(parsed.release->assets.size()), before + zips.size());
        for (const PackageKind kind : {PackageKind::MacBundle, PackageKind::WindowsInstall, PackageKind::Deb}) {
            const QString name = assetNameFor(kind, parsed.release->version);
            const ReleaseAsset* asset = parsed.release->asset(name);
            QVERIFY2(asset, qPrintable(name));
            QCOMPARE(asset->name, name);
            QVERIFY(!asset->name.endsWith(QLatin1String(".zip")));
        }
        QVERIFY(assetNameFor(PackageKind::Unknown, parsed.release->version).isEmpty());
    }

    void noAssetsIsStillARelease() {
        QJsonObject object = fixtureObject("release-v0.1.1.json");
        object.remove(QStringLiteral("assets"));
        const ParsedRelease parsed = parseRelease(toJson(object), kRepo);
        QVERIFY(parsed.release);
        QVERIFY(parsed.release->assets.empty());
    }

    // ---- checksumFor ------------------------------------------------------

    void checksumFor_data() {
        QTest::addColumn<QByteArray>("file");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("expected");
        const QByteArray hex = kHex.toLatin1();
        const QByteArray other = QByteArray("f").repeated(64);
        const QByteArray sums =
            hex + "  GitBolt-0.1.1-Darwin-arm64.dmg\n" + other + "  GitBolt-0.1.1-x86_64.AppImage\n";
        QTest::newRow("first line") << sums << "GitBolt-0.1.1-Darwin-arm64.dmg" << hex;
        QTest::newRow("second line") << sums << "GitBolt-0.1.1-x86_64.AppImage" << other;
        QTest::newRow("no such name") << sums << "gitbolt_0.1.1_amd64.deb" << QByteArray();
        QTest::newRow("empty name") << sums << "" << QByteArray();
        QTest::newRow("a prefix of a name") << sums << "GitBolt-0.1.1-Darwin-arm64" << QByteArray();
        QTest::newRow("a suffix of a name") << sums << "arm64.dmg" << QByteArray();
        QTest::newRow("binary mode") << hex + " *GitBolt.exe\n" << "GitBolt.exe" << hex;
        QTest::newRow("CRLF") << hex + "  GitBolt.exe\r\n" << "GitBolt.exe" << hex;
        QTest::newRow("no final newline") << hex + "  GitBolt.exe" << "GitBolt.exe" << hex;
        QTest::newRow("upper case, lowered") << hex.toUpper() + "  GitBolt.exe\n" << "GitBolt.exe" << hex;
        QTest::newRow("one space") << hex + " GitBolt.exe\n" << "GitBolt.exe" << QByteArray();
        QTest::newRow("a tab") << hex + "\tGitBolt.exe\n" << "GitBolt.exe" << QByteArray();
        QTest::newRow("short hash") << hex.left(63) + "  GitBolt.exe\n" << "GitBolt.exe" << QByteArray();
        QTest::newRow("not hex") << QByteArray("g").repeated(64) + "  GitBolt.exe\n" << "GitBolt.exe" << QByteArray();
        QTest::newRow("trailing space") << hex + "  GitBolt.exe \n" << "GitBolt.exe" << QByteArray();
        QTest::newRow("escaped line") << "\\" + hex + "  GitBolt.exe\n" << "GitBolt.exe" << QByteArray();
        QTest::newRow("same twice") << hex + "  GitBolt.exe\n" + hex + "  GitBolt.exe\n" << "GitBolt.exe" << hex;
        QTest::newRow("two answers") << hex + "  GitBolt.exe\n" + other + "  GitBolt.exe\n" << "GitBolt.exe"
                                     << QByteArray();
        QTest::newRow("empty file") << QByteArray() << "GitBolt.exe" << QByteArray();
        QTest::newRow("a path, not the name") << hex + "  dist/GitBolt.exe\n" << "GitBolt.exe" << QByteArray();
        // A symbols zip's line, named like the installer and more, never
        // answers for the installer's name.
        QTest::newRow("a symbols zip beside it")
            << hex + "  GitBolt-0.1.1-Darwin-arm64.dmg\n" + other + "  GitBolt-0.1.1-Darwin-arm64-symbols.zip\n"
            << "GitBolt-0.1.1-Darwin-arm64.dmg" << hex;
    }
    void checksumFor() {
        QFETCH(QByteArray, file);
        QFETCH(QString, name);
        QFETCH(QByteArray, expected);
        QCOMPARE(gitbolt::update::checksumFor(file, name), expected);
    }

    // ---- systemTooOld -----------------------------------------------------

    void systemTooOld_data() {
        QTest::addColumn<QString>("minimum");
        QTest::addColumn<QString>("running");
        QTest::addColumn<bool>("tooOld");
        QTest::newRow("Windows: an older build") << "10.0.22000" << "10.0.19045" << true;
        QTest::newRow("Windows: that build") << "10.0.17763" << "10.0.17763" << false;
        QTest::newRow("Windows: a newer build") << "10.0.17763" << "10.0.26100" << false;
        QTest::newRow("macOS: 14.4 is 14.4.0") << "14.4" << "14.4.0" << false;
        QTest::newRow("macOS: older") << "14.4" << "14.3.1" << true;
        QTest::newRow("macOS: newer") << "14.4" << "15.0.0" << false;
        QTest::newRow("spaces around it") << " 14.4 " << "14.3.1" << true;
        QTest::newRow("no minimum") << "" << "10.0.17763" << false;
        QTest::newRow("not a version") << "garbage" << "10.0.17763" << false;
        QTest::newRow("a version and more") << "99.0 beta" << "10.0.17763" << false;
    }
    void systemTooOld() {
        QFETCH(QString, minimum);
        QFETCH(QString, running);
        QFETCH(bool, tooOld);
        QCOMPARE(gitbolt::update::systemTooOld(minimum, QVersionNumber::fromString(running)), tooOld);
    }
};

QTEST_GUILESS_MAIN(TestUpdateRules)
#include "TestUpdateRules.moc"
