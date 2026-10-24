//
// TestGitNotice — the strip above the views that says what is wrong
// with the git GitBolt runs (widgets/GitNoticeBar.h).
//
// GitBolt ran whatever git it found first, and said nothing about it:
// Apple's 2.39.5, open to CVE-2025-48384, or a git too old for the
// --end-of-options its commands rely on. Now:
//
//   - a git with known security flaws gets a note that names them, the
//     git and where it is, what the flaws let a repository do, and the
//     fix for that kind of git (Apple's tools or Homebrew, Git for
//     Windows, git);
//   - one older than GitBolt's floor gets a note that says so, on Linux
//     too, where distributions backport fixes and versions say nothing
//     more;
//   - no git, or Apple's stand-in without the tools behind it, gets a
//     note too; a git that is fine, or didn't say, gets none;
//   - the note is plain text (the path is the disk's);
//   - Dismiss hides it, and the window doesn't show it again for that
//     git and version, but does for another.
//

#include <QDir>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "conf/SettingsService.h"
#include "git/GitVersion.h"
#include "ui/MainWindow.h"
#include "widgets/GitNoticeBar.h"

using gitbolt::git::GitPlatform;
using gitbolt::git::GitProbe;
using gitbolt::widgets::GitNoticeBar;

Q_DECLARE_METATYPE(GitPlatform)
Q_DECLARE_METATYPE(GitProbe)

namespace {

// As in TestGitVersion: one advisory that runs code, for every build,
// and one Windows-only that leaks credentials.
const QByteArray kTable = R"({
  "checked": "2026-10-24",
  "floor": "2.24.0",
  "advisories": [
    {"ids": ["CVE-A"], "risk": "code", "builds": ["upstream", "apple", "windows"],
     "fixed": ["2.43.7", "2.50.1"], "apple_fixed": 155},
    {"ids": ["CVE-W"], "risk": "credentials", "builds": ["windows"],
     "fixed": ["2.55.0.windows.4"]}
  ]
})";

GitProbe probe(const QString& path, const char* version, bool toolsMissing = false) {
    GitProbe found;
    found.path = path;
    if (version)
        found.version = gitbolt::git::parseGitVersion(QString::fromLatin1(version));
    found.appleToolsMissing = toolsMissing;
    return found;
}

} // namespace

class TestGitNotice : public QObject {
    Q_OBJECT

    QTemporaryDir settingsDir_;

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    void says_data() {
        QTest::addColumn<GitProbe>("found");
        QTest::addColumn<GitPlatform>("platform");
        // What the note must say, a fragment each; none: no note.
        QTest::addColumn<QStringList>("says");
        QTest::addColumn<QString>("key");

        const QString usrBin = QStringLiteral("/usr/bin/git");
        const QString programFiles = QStringLiteral("C:/Program Files/Git/cmd/git.exe");
        QTest::newRow("apple, flawed")
            << probe(usrBin, "git version 2.39.5 (Apple Git-154)") << GitPlatform::MacOS
            << QStringList{"git 2.39.5 (Apple Git-154)", "/usr/bin/git", "(CVE-A)",
                           "could run programs", "Command Line Tools", "brew install git"}
            << "/usr/bin/git\n2.39.5 (Apple Git-154)\nCVE-A";
        QTest::newRow("apple, fixed by its build")
            << probe(usrBin, "git version 2.39.5 (Apple Git-155)") << GitPlatform::MacOS
            << QStringList() << QString();
        QTest::newRow("homebrew's, flawed")
            << probe(QStringLiteral("/opt/homebrew/bin/git"), "git version 2.43.0")
            << GitPlatform::MacOS << QStringList{"(CVE-A)", "brew upgrade git"}
            << "/opt/homebrew/bin/git\n2.43.0\nCVE-A";
        QTest::newRow("windows, credentials")
            << probe(programFiles, "git version 2.55.0.windows.3") << GitPlatform::Windows
            << QStringList{"(CVE-W)", "Windows password hash", "Update Git for Windows",
                           QDir::toNativeSeparators(programFiles)}
            << programFiles + "\n2.55.0.windows.3\nCVE-W";
        // Both: named in the table's order, and the worse risk.
        QTest::newRow("windows, both")
            << probe(programFiles, "git version 2.45.2.windows.1") << GitPlatform::Windows
            << QStringList{"(CVE-A and CVE-W)", "could run programs"}
            << programFiles + "\n2.45.2.windows.1\nCVE-A\nCVE-W";
        QTest::newRow("linux, too old")
            << probe(usrBin, "git version 2.20.1") << GitPlatform::Other
            << QStringList{"git 2.20.1", "too old", "git 2.24 or later", "Update git"}
            << "/usr/bin/git\n2.20.1\nfloor";
        QTest::newRow("linux, a version that backports")
            << probe(usrBin, "git version 2.39.5") << GitPlatform::Other
            << QStringList() << QString();
        QTest::newRow("no git") << probe(QString(), nullptr) << GitPlatform::Other
                                << QStringList{"didn't find git"} << "\nmissing";
        QTest::newRow("apple's tools missing")
            << probe(usrBin, nullptr, true) << GitPlatform::MacOS
            << QStringList{"Command Line Tools", "xcode-select --install"}
            << "/usr/bin/git\ntools missing";
        QTest::newRow("didn't say") << probe(usrBin, nullptr) << GitPlatform::MacOS
                                    << QStringList() << QString();
    }

    void says() {
        QFETCH(GitProbe, found);
        QFETCH(GitPlatform, platform);
        QFETCH(QStringList, says);
        QFETCH(QString, key);
        const auto notice = GitNoticeBar::noticeFor(found, platform,
                                                    *gitbolt::git::parseGitAdvisories(kTable));
        QCOMPARE(notice.has_value(), !says.isEmpty());
        if (!notice)
            return;
        for (const QString& fragment : says)
            QVERIFY2(notice->text.contains(fragment), qPrintable(notice->text));
        QCOMPARE(notice->key, key);
    }

    // The path is shown as it is, whatever it looks like.
    void plainText() {
        GitNoticeBar bar;
        bar.showNotice({QStringLiteral("k"), QStringLiteral("git 2.39.5 at /tmp/<b>x</b>/git")});
        auto* message = bar.findChild<QLabel*>(QStringLiteral("gitNotice.message"));
        QVERIFY(message);
        QCOMPARE(message->textFormat(), Qt::PlainText);
        QCOMPARE(message->text(), QStringLiteral("git 2.39.5 at /tmp/<b>x</b>/git"));
    }

    void dismissHidesItAndNamesIt() {
        GitNoticeBar bar;
        QVERIFY(bar.isHidden());
        QString dismissed;
        connect(&bar, &GitNoticeBar::dismissed, this,
                [&dismissed](const QString& key) { dismissed = key; });
        bar.showNotice({QStringLiteral("/usr/bin/git\n2.20.1\nfloor"), QStringLiteral("old")});
        QVERIFY(!bar.isHidden());
        auto* dismiss = bar.findChild<QPushButton*>(QStringLiteral("gitNotice.dismiss"));
        QVERIFY(dismiss);
        dismiss->click();
        QVERIFY(bar.isHidden());
        QCOMPARE(dismissed, QStringLiteral("/usr/bin/git\n2.20.1\nfloor"));
    }

    // In the window: above the views, and gone for good once dismissed,
    // until the git or its version changes. Below the floor, so there is
    // a note on every platform (on macOS and Windows about its flaws).
    void windowRemembersADismissal() {
        GitProbe old = probe(QStringLiteral("/opt/gb-test/bin/git"), "git version 2.20.1");
        {
            gitbolt::ui::MainWindow window;
            auto* bar = window.findChild<GitNoticeBar*>();
            QVERIFY(bar);
            QVERIFY(bar->isHidden());
            QCOMPARE(window.centralWidget()->layout()->itemAt(0)->widget(), bar);
            window.showGitNotice(old);
            QVERIFY(!bar->isHidden());
            QVERIFY2(bar->text().contains(QStringLiteral("git 2.20.1")), qPrintable(bar->text()));
            auto* dismiss = bar->findChild<QPushButton*>(QStringLiteral("gitNotice.dismiss"));
            QVERIFY(dismiss);
            dismiss->click();
            QVERIFY(bar->isHidden());
        }
        gitbolt::ui::MainWindow window;
        auto* bar = window.findChild<GitNoticeBar*>();
        QVERIFY(bar);
        window.showGitNotice(old);
        QVERIFY(bar->isHidden());
        // Another version of that git: a note of its own.
        old.version = gitbolt::git::parseGitVersion(QStringLiteral("git version 2.17.1"));
        window.showGitNotice(old);
        QVERIFY(!bar->isHidden());
        // And a git that is fine (one from the future) takes it away.
        old.version = gitbolt::git::parseGitVersion(QStringLiteral("git version 2.99.0"));
        window.showGitNotice(old);
        QVERIFY(bar->isHidden());
    }
};

QTEST_MAIN(TestGitNotice)
#include "TestGitNotice.moc"
