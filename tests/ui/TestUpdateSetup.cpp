//
// TestUpdateSetup — the in-app updates gate main() evaluates once
// (ui/UpdateSetup.h), as a table, with the environment injected:
//
//   - a test session never gets the official feed, whatever the build:
//     off without GITBOLT_TEST_UPDATE_FEED, on against it only when it
//     is plain http to 127.0.0.1 or [::1] (https to GitHub, localhost,
//     another address, user info or garbage are refused), with loopback
//     downloads allowed and GITBOLT_TEST_APP_VERSION as the version;
//   - outside a test session the test variables are ignored (and the
//     log says so), and only a build with the official feed is on;
//   - GITBOLT_UPDATE_CHECK=0 keeps the setup on but schedules nothing;
//   - the Config is usable when on: downloads under <cache>/updates (a
//     test session's under its temporary folder, never the user's
//     cache: macOS's ignores HOME), the repository's releases,
//     telemetry's User-Agent with the version.
//
// The official feed here is a stand-in on api.github.com that nothing
// fetches: no request leaves this test.
//

#include <QDir>
#include <QTest>

#include "ui/UpdateSetup.h"

using gitbolt::ui::UpdateGate;
using gitbolt::ui::UpdateSetup;
using gitbolt::ui::UpdateSetupInputs;
using gitbolt::util::TestSessionKind;

Q_DECLARE_METATYPE(UpdateGate)
Q_DECLARE_METATYPE(TestSessionKind)

namespace {

// Never fetched: no test sends anything.
const QUrl kStandInFeed(QStringLiteral("https://api.github.com/repos/Inverian/GitBolt/releases/test-stand-in"));
const QString kLoopbackFeed = QStringLiteral("http://127.0.0.1:41234/repos/Inverian/GitBolt/releases/latest");

QProcessEnvironment env(const QStringList& pairs) {
    QProcessEnvironment e;
    for (const QString& pair : pairs)
        e.insert(pair.section(QLatin1Char('='), 0, 0), pair.section(QLatin1Char('='), 1));
    return e;
}

QString cacheDir() {
    return QDir::temp().filePath(QStringLiteral("gitbolt-update-setup-test-cache"));
}

QString tempDir() {
    return QDir::temp().filePath(QStringLiteral("gitbolt-update-setup-test-temp"));
}

} // namespace

class TestUpdateSetup : public QObject {
    Q_OBJECT

private slots:
    void gate_data() {
        QTest::addColumn<bool>("official");
        QTest::addColumn<TestSessionKind>("session");
        QTest::addColumn<QStringList>("environment");
        QTest::addColumn<UpdateGate>("gate");

        const auto none = TestSessionKind::None;
        const auto bridge = TestSessionKind::Bridge;
        const QString feed = QStringLiteral("GITBOLT_TEST_UPDATE_FEED=") + kLoopbackFeed;

        QTest::newRow("source build") << false << none << QStringList{} << UpdateGate::CompiledOut;
        QTest::newRow("official build") << true << none << QStringList{} << UpdateGate::Active;
        QTest::newRow("official, opted out") << true << none << QStringList{"GITBOLT_UPDATE_CHECK=0"}
                                             << UpdateGate::Active;

        // A test session, official build or not, never asks the feed.
        const QList<QPair<TestSessionKind, QByteArray>> kinds{{TestSessionKind::Bridge, "bridge"},
                                                              {TestSessionKind::Instance, "instance name"},
                                                              {TestSessionKind::Headless, "headless"},
                                                              {TestSessionKind::Ci, "CI"}};
        for (const auto& [kind, name] : kinds) {
            QTest::newRow(("test session " + name + ", official").constData())
                << true << kind << QStringList{} << UpdateGate::TestSession;
            QTest::newRow(("test session " + name + ", test feed").constData())
                << false << kind << QStringList{feed} << UpdateGate::TestFeed;
        }
        QTest::newRow("test feed, official build") << true << bridge << QStringList{feed} << UpdateGate::TestFeed;
        QTest::newRow("test feed on ::1")
            << false << bridge << QStringList{"GITBOLT_TEST_UPDATE_FEED=http://[::1]:8080/feed"}
            << UpdateGate::TestFeed;
        QTest::newRow("test feed and version")
            << false << bridge << QStringList{feed, "GITBOLT_TEST_APP_VERSION=0.1.0"} << UpdateGate::TestFeed;

        // Refused: only plain http to loopback, by address.
        for (const char* bad : {"https://api.github.com/repos/Inverian/GitBolt/releases/latest",
                                "http://localhost:8080/feed", "http://127.0.0.2:8080/feed",
                                "http://user:pw@127.0.0.1:8080/feed", "https://127.0.0.1:8080/feed",
                                "ftp://127.0.0.1/feed", "not a url", ""}) {
            QTest::newRow((QByteArray("refused: ") + bad).constData())
                << true << bridge << QStringList{QStringLiteral("GITBOLT_TEST_UPDATE_FEED=") + QLatin1String(bad)}
                << UpdateGate::TestFeedRefused;
        }
        for (const char* bad : {"0.1", "0.1.0-rc1", "0.0.0", "01.1.0", "x"}) {
            QTest::newRow((QByteArray("bad version: ") + bad).constData())
                << false << bridge
                << QStringList{feed, QStringLiteral("GITBOLT_TEST_APP_VERSION=") + QLatin1String(bad)}
                << UpdateGate::TestFeedRefused;
        }

        // Outside a test session the test variables do nothing.
        QTest::newRow("test feed outside a test session") << false << none << QStringList{feed}
                                                          << UpdateGate::CompiledOut;
        QTest::newRow("test feed outside a test session, official")
            << true << none << QStringList{feed, "GITBOLT_TEST_APP_VERSION=0.1.0"} << UpdateGate::Active;
    }

    void gate() {
        QFETCH(bool, official);
        QFETCH(TestSessionKind, session);
        QFETCH(QStringList, environment);
        QFETCH(UpdateGate, gate);

        UpdateSetupInputs inputs;
        if (official)
            inputs.officialFeed = kStandInFeed;
        inputs.testSession = session;
        inputs.env = env(environment);
        inputs.appVersion = QStringLiteral("0.1.5");
        inputs.cacheDir = cacheDir();
        inputs.tempDir = tempDir();
        const UpdateSetup setup = gitbolt::ui::setUpUpdates(inputs);
        QCOMPARE(setup.gate, gate);

        const bool on = gate == UpdateGate::TestFeed || gate == UpdateGate::Active;
        QCOMPARE(setup.on(), on);
        QCOMPARE(setup.config.isUsable(), on);
        if (gate == UpdateGate::TestFeedRefused)
            QVERIFY(!setup.problem.isEmpty());
        if (!on)
            return;

        // The feed: the test's in a test session, the official one only
        // outside it.
        if (gate == UpdateGate::TestFeed) {
            QVERIFY(gitbolt::update::isLoopbackHttpUrl(setup.config.feedUrl));
            QVERIFY(setup.config.allowLoopback);
            QVERIFY(!setup.schedulesChecks());
            const bool versioned = environment.join(QLatin1Char(' ')).contains(QLatin1String("GITBOLT_TEST_APP_VERSION"));
            QCOMPARE(setup.config.current.toString(), versioned ? QStringLiteral("0.1.0") : QStringLiteral("0.1.5"));
            QCOMPARE(setup.config.downloadDir, QDir(tempDir()).filePath(QStringLiteral("gitbolt-updates")));
        } else {
            QCOMPARE(setup.config.feedUrl, kStandInFeed);
            QVERIFY(!setup.config.allowLoopback);
            QCOMPARE(setup.config.current.toString(), QStringLiteral("0.1.5"));
            QCOMPARE(setup.schedulesChecks(), !environment.contains(QLatin1String("GITBOLT_UPDATE_CHECK=0")));
            QCOMPARE(setup.envOptOut, environment.contains(QLatin1String("GITBOLT_UPDATE_CHECK=0")));
            QCOMPARE(setup.config.downloadDir, QDir(cacheDir()).filePath(QStringLiteral("updates")));
        }
        QCOMPARE(setup.config.repo, QStringLiteral("Inverian/GitBolt"));
        QVERIFY2(setup.config.userAgent.startsWith(
                     QStringLiteral("GitBolt/%1 (").arg(setup.config.current.toString())),
                 qPrintable(setup.config.userAgent));
        // The production rules for downloads, loopback aside.
        QVERIFY(setup.config.isAllowedDownloadUrl(
            QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/download/v0.1.6/x.dmg"))));
    }

    // The log says when a test variable was refused or ignored.
    void saysWhyAVariableIsIgnored() {
        UpdateSetupInputs inputs;
        inputs.env = env({QStringLiteral("GITBOLT_TEST_UPDATE_FEED=") + kLoopbackFeed});
        inputs.appVersion = QStringLiteral("0.1.5");
        inputs.cacheDir = cacheDir();
        inputs.tempDir = tempDir();
        QVERIFY(gitbolt::ui::setUpUpdates(inputs).problem.contains(QLatin1String("test sessions only")));

        inputs.testSession = TestSessionKind::Instance;
        QVERIFY(gitbolt::ui::setUpUpdates(inputs).problem.isEmpty());
        inputs.env = env({QStringLiteral("GITBOLT_TEST_UPDATE_FEED=https://api.github.com/x")});
        QVERIFY(gitbolt::ui::setUpUpdates(inputs).problem.contains(QLatin1String("127.0.0.1")));
    }

    // Without a cache folder (a test session: a temporary one) there's
    // nowhere to download to: off.
    void needsADownloadFolder() {
        UpdateSetupInputs inputs;
        inputs.officialFeed = kStandInFeed;
        inputs.appVersion = QStringLiteral("0.1.5");
        const UpdateSetup official = gitbolt::ui::setUpUpdates(inputs);
        QCOMPARE(official.gate, UpdateGate::CompiledOut);
        QVERIFY(!official.problem.isEmpty());

        inputs.testSession = TestSessionKind::Bridge;
        inputs.env = env({QStringLiteral("GITBOLT_TEST_UPDATE_FEED=") + kLoopbackFeed});
        inputs.cacheDir = cacheDir();   // never a test session's
        QCOMPARE(gitbolt::ui::setUpUpdates(inputs).gate, UpdateGate::TestFeedRefused);
    }

    void gateNames() {
        QCOMPARE(gitbolt::ui::updateGateName(UpdateGate::CompiledOut), QStringLiteral("compiled-out"));
        QCOMPARE(gitbolt::ui::updateGateName(UpdateGate::TestSession), QStringLiteral("off:test-session"));
        QCOMPARE(gitbolt::ui::updateGateName(UpdateGate::TestFeedRefused), QStringLiteral("off:test-feed-refused"));
        QCOMPARE(gitbolt::ui::updateGateName(UpdateGate::TestFeed), QStringLiteral("test-feed"));
        QCOMPARE(gitbolt::ui::updateGateName(UpdateGate::Active), QStringLiteral("on"));
    }
};

QTEST_GUILESS_MAIN(TestUpdateSetup)
#include "TestUpdateSetup.moc"
