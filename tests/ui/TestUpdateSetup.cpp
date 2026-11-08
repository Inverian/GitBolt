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
//     telemetry's User-Agent with the version;
//   - a test session's downloads folder is its own: a new
//     gitbolt-updates-XXXXXX in the temporary folder, 0700, never the
//     shared gitbolt-updates another user could have made first; the
//     one GITBOLT_TEST_UPDATE_DIR hands on (to the relaunched copy) is
//     kept only while it is still a private folder of this user's in
//     the temporary folder, never a link, a folder others may write to
//     or one elsewhere.
//
// The official feed here is a stand-in on api.github.com that nothing
// fetches: no request leaves this test.
//

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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

// The session's temporary folder: a real one, as test-feed sessions
// make their downloads folder in it.
QTemporaryDir& tempRoot() {
    static QTemporaryDir dir;
    return dir;
}

QString tempDir() {
    return QDir::cleanPath(tempRoot().path());
}

// A test-feed session in `temp`, handed `handedOn` (empty: nothing).
UpdateSetup testFeedSetup(const QString& temp, const QString& handedOn = {}) {
    UpdateSetupInputs inputs;
    inputs.testSession = TestSessionKind::Bridge;
    inputs.env = env({QStringLiteral("GITBOLT_TEST_UPDATE_FEED=") + kLoopbackFeed});
    if (!handedOn.isEmpty())
        inputs.env.insert(QString::fromLatin1(gitbolt::ui::kTestUpdateDirEnv), handedOn);
    inputs.appVersion = QStringLiteral("0.1.5");
    inputs.cacheDir = cacheDir();
    inputs.tempDir = temp;
    return gitbolt::ui::setUpUpdates(inputs);
}

// Only this user may enter it (Unix), and it's a fresh folder of the
// session's own, directly in `temp`.
void verifyPrivateFolder(const QString& dir, const QString& temp) {
    const QFileInfo info(dir);
    QVERIFY2(info.isDir() && !info.isSymLink(), qPrintable(dir));
    QCOMPARE(QDir::cleanPath(info.absolutePath()), QDir::cleanPath(temp));
    QVERIFY2(info.fileName().startsWith(QLatin1String("gitbolt-updates-")), qPrintable(dir));
#if defined(Q_OS_UNIX)
    QCOMPARE(QFile::permissions(dir) & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther
                                        | QFile::WriteOther | QFile::ExeOther),
             QFile::Permissions{});
#endif
}

} // namespace

class TestUpdateSetup : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(tempRoot().isValid());
    }

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
            verifyPrivateFolder(setup.config.downloadDir, tempDir());
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

        // A temporary folder it can't make its own folder in: off, and
        // the log says why.
        const UpdateSetup nowhere = testFeedSetup(QDir(tempDir()).filePath(QStringLiteral("missing/deeper")));
        QCOMPARE(nowhere.gate, UpdateGate::TestFeedRefused);
        QVERIFY2(nowhere.problem.contains(QLatin1String("private folder")), qPrintable(nowhere.problem));
    }

    // ---- a test session's downloads folder ----------------------------------

    // Each session makes its own, never the fixed gitbolt-updates: on
    // Linux without TMPDIR the temporary folder is the shared /tmp, where
    // another user could have made that first, open to all.
    void aTestFeedSessionDownloadsIntoAFolderOfItsOwn() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString shared = QDir(temp.path()).filePath(QStringLiteral("gitbolt-updates"));
        QVERIFY(QDir().mkpath(shared));
#if defined(Q_OS_UNIX)
        QVERIFY(QFile::setPermissions(shared, QFile::Permissions(0x7777)));   // rwx for all
#endif

        const UpdateSetup one = testFeedSetup(temp.path());
        const UpdateSetup two = testFeedSetup(temp.path());
        QCOMPARE(one.gate, UpdateGate::TestFeed);
        QCOMPARE(two.gate, UpdateGate::TestFeed);
        verifyPrivateFolder(one.config.downloadDir, temp.path());
        verifyPrivateFolder(two.config.downloadDir, temp.path());
        QVERIFY(one.config.downloadDir != two.config.downloadDir);
        QVERIFY(one.config.downloadDir != shared);
        QVERIFY(one.problem.isEmpty());

        // A session that is off makes nothing.
        QTemporaryDir untouched;
        UpdateSetupInputs off;
        off.testSession = TestSessionKind::Bridge;
        off.appVersion = QStringLiteral("0.1.5");
        off.tempDir = untouched.path();
        QCOMPARE(gitbolt::ui::setUpUpdates(off).gate, UpdateGate::TestSession);
        off.env = env({QStringLiteral("GITBOLT_TEST_UPDATE_FEED=http://localhost:1/feed")});
        QCOMPARE(gitbolt::ui::setUpUpdates(off).gate, UpdateGate::TestFeedRefused);
        QCOMPARE(QDir(untouched.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{});
    }

    // The copy the install helper relaunches inherits the folder (main()
    // hands it on in GITBOLT_TEST_UPDATE_DIR), where the helper left its
    // install-result.
    void theRelaunchedCopyKeepsTheFolder() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const UpdateSetup first = testFeedSetup(temp.path());
        QCOMPARE(first.gate, UpdateGate::TestFeed);
        const UpdateSetup relaunched = testFeedSetup(temp.path(), first.config.downloadDir);
        QCOMPARE(relaunched.gate, UpdateGate::TestFeed);
        QCOMPARE(relaunched.config.downloadDir, first.config.downloadDir);
        // Outside a test feed session the variable means nothing.
        UpdateSetupInputs official;
        official.officialFeed = kStandInFeed;
        official.env.insert(QString::fromLatin1(gitbolt::ui::kTestUpdateDirEnv), first.config.downloadDir);
        official.appVersion = QStringLiteral("0.1.5");
        official.cacheDir = cacheDir();
        official.tempDir = temp.path();
        QCOMPARE(gitbolt::ui::setUpUpdates(official).config.downloadDir,
                 QDir(cacheDir()).filePath(QStringLiteral("updates")));
    }

    // A folder handed on that isn't (or is no longer) private to this
    // user, or isn't one of these, is left alone: the session makes a
    // new one.
    void anUnsafeHandedOnFolderIsntUsed_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("missing") << "missing";
        QTest::newRow("relative") << "relative";
        QTest::newRow("another name") << "another name";
        QTest::newRow("elsewhere") << "elsewhere";
        QTest::newRow("a file") << "file";
#if defined(Q_OS_UNIX)
        QTest::newRow("a link to a private folder") << "link";
        QTest::newRow("others may write") << "0777";
        QTest::newRow("the group may write") << "0770";
        QTest::newRow("others may look") << "0755";
#endif
    }
    void anUnsafeHandedOnFolderIsntUsed() {
        QFETCH(QString, kind);
        QTemporaryDir temp;
        QTemporaryDir elsewhere;
        QVERIFY(temp.isValid() && elsewhere.isValid());
        const QDir in(temp.path());
        QString handed = in.filePath(QStringLiteral("gitbolt-updates-handed"));
        if (kind == QLatin1String("missing")) {
            // nothing there
        } else if (kind == QLatin1String("relative")) {
            QVERIFY(in.mkdir(QStringLiteral("gitbolt-updates-handed")));
            handed = QStringLiteral("gitbolt-updates-handed");
        } else if (kind == QLatin1String("another name")) {
            handed = in.filePath(QStringLiteral("other"));
            QVERIFY(QDir().mkdir(handed, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        } else if (kind == QLatin1String("elsewhere")) {
            handed = QDir(elsewhere.path()).filePath(QStringLiteral("gitbolt-updates-handed"));
            QVERIFY(QDir().mkdir(handed, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        } else if (kind == QLatin1String("file")) {
            QFile file(handed);
            QVERIFY(file.open(QIODevice::WriteOnly));
        } else if (kind == QLatin1String("link")) {
            const QString target = QDir(elsewhere.path()).filePath(QStringLiteral("private"));
            QVERIFY(QDir().mkdir(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            QVERIFY(QFile::link(target, handed));
        } else {
            QVERIFY(in.mkdir(QStringLiteral("gitbolt-updates-handed")));
            const int mode = kind.toInt(nullptr, 8);
            QFile::Permissions perms;
            // QFile's flags by octal digit: owner, group, others.
            const QFile::Permission bits[] = {QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner,
                                              QFile::ReadGroup, QFile::WriteGroup, QFile::ExeGroup,
                                              QFile::ReadOther, QFile::WriteOther, QFile::ExeOther};
            for (int i = 0; i < 9; ++i) {
                if (mode & (0400 >> i))
                    perms |= bits[i];
            }
            QVERIFY(QFile::setPermissions(handed, perms));
        }

        const UpdateSetup setup = testFeedSetup(temp.path(), handed);
        QCOMPARE(setup.gate, UpdateGate::TestFeed);
        QVERIFY(setup.config.downloadDir != handed);
        QVERIFY(setup.config.downloadDir != in.filePath(QStringLiteral("gitbolt-updates-handed")));
        verifyPrivateFolder(setup.config.downloadDir, temp.path());
    }

    // Only the private folder itself counts: one this user made 0700,
    // directly in the temporary folder, keeps being used.
    void aPrivateHandedOnFolderIsUsed() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString handed = QDir(temp.path()).filePath(QStringLiteral("gitbolt-updates-handed"));
        QVERIFY(QDir().mkdir(handed, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCOMPARE(testFeedSetup(temp.path(), handed).config.downloadDir, handed);
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
