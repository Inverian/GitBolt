//
// TestGitVersion — what GitBolt makes of the git it runs (git/GitVersion.h).
//
// GitBolt runs the user's git for clones, fetches, commits and rebases,
// and Apple's /usr/bin/git, which a Mac started from the Dock used to get
// whatever the user's shell ran, was 2.39.5: open to CVE-2025-48384, in
// CISA's catalog of exploited vulnerabilities. GitBolt now says so:
//
//   - `git --version` is taken apart: git's own builds, Apple's ("Apple
//     Git-N"), Git for Windows' (".windows.N") and anyone else's;
//   - a version is affected by an advisory below the fixed release of
//     its line, on a line between or before the fixed ones, and not on a
//     newer one; an Apple build also by its Apple Git-N;
//   - on Linux only a git older than the floor is worth a word, since
//     distributions backport fixes and keep the version;
//   - the table built into GitBolt reads, and says what it should about
//     the gits users have (Apple Git-154, Git for Windows lines);
//   - a malformed table is refused whole, never half read.
//

#include <QTest>

#include "git/GitVersion.h"

using gitbolt::git::GitAdvisories;
using gitbolt::git::GitPlatform;
using gitbolt::git::GitVersion;

Q_DECLARE_METATYPE(GitPlatform)

namespace {

// Two advisories with what the semantics need: lines with a fix, a
// line between them, an Apple build number, and one Windows-only.
const QByteArray kTable = R"({
  "checked": "2026-10-24",
  "floor": "2.24.0",
  "advisories": [
    {"ids": ["CVE-A"], "risk": "code", "builds": ["upstream", "apple", "windows"],
     "fixed": ["2.43.7", "2.50.1"], "apple_fixed": 155},
    {"ids": ["CVE-W"], "risk": "credentials", "builds": ["windows"],
     "fixed": ["2.53.0.windows.4", "2.55.0.windows.4"]}
  ]
})";

GitAdvisories table() {
    return *gitbolt::git::parseGitAdvisories(kTable);
}

// The first ID of each advisory `output` is affected by on `platform`,
// with "floor" first when it is older than the floor.
QStringList verdict(const QString& output, GitPlatform platform, const GitAdvisories& advisories) {
    const auto version = gitbolt::git::parseGitVersion(output);
    if (!version)
        return {QStringLiteral("unparsed")};
    const auto assessment = gitbolt::git::assessGitVersion(*version, platform, advisories);
    QStringList ids;
    if (assessment.belowFloor)
        ids << QStringLiteral("floor");
    for (const auto* advisory : assessment.advisories)
        ids << advisory->ids.first();
    return ids;
}

} // namespace

class TestGitVersion : public QObject {
    Q_OBJECT

private slots:
    void parses_data() {
        QTest::addColumn<QString>("output");
        QTest::addColumn<int>("major");
        QTest::addColumn<int>("minor");
        QTest::addColumn<int>("patch");
        QTest::addColumn<int>("build");
        QTest::addColumn<int>("number");   // Apple Git-N or .windows.N
        QTest::addColumn<QString>("text");

        const int upstream = int(GitVersion::Build::Upstream);
        const int apple = int(GitVersion::Build::Apple);
        const int windows = int(GitVersion::Build::Windows);
        const int other = int(GitVersion::Build::Other);
        QTest::newRow("upstream") << "git version 2.56.0\n" << 2 << 56 << 0 << upstream << 0
                                  << "2.56.0";
        QTest::newRow("no patch") << "git version 2.24" << 2 << 24 << 0 << upstream << 0 << "2.24";
        QTest::newRow("release candidate")
            << "git version 2.51.0-rc1" << 2 << 51 << 0 << upstream << 0 << "2.51.0-rc1";
        QTest::newRow("from a checkout") << "git version 2.52.0.153.g1a2b3c4.dirty" << 2 << 52 << 0
                                         << upstream << 0 << "2.52.0.153.g1a2b3c4.dirty";
        QTest::newRow("apple") << "git version 2.39.5 (Apple Git-154)" << 2 << 39 << 5 << apple
                               << 154 << "2.39.5 (Apple Git-154)";
        QTest::newRow("git for windows") << "git version 2.53.0.windows.3" << 2 << 53 << 0
                                         << windows << 3 << "2.53.0.windows.3";
        QTest::newRow("git for windows rc") << "git version 2.56.0.rc2.windows.1" << 2 << 56 << 0
                                            << windows << 1 << "2.56.0.rc2.windows.1";
        QTest::newRow("microsoft's") << "git version 2.45.2.vfs.0.4" << 2 << 45 << 2 << other << 0
                                     << "2.45.2.vfs.0.4";
    }

    void parses() {
        QFETCH(QString, output);
        const auto version = gitbolt::git::parseGitVersion(output);
        QVERIFY(version);
        QTEST(version->major, "major");
        QTEST(version->minor, "minor");
        QTEST(version->patch, "patch");
        QTEST(int(version->build), "build");
        QFETCH(int, number);
        QCOMPARE(version->build == GitVersion::Build::Apple ? version->appleBuild
                                                            : version->windowsRevision,
                 number);
        QTEST(version->text, "text");
    }

    void refusesWhatIsntAVersion_data() {
        QTest::addColumn<QString>("output");
        QTest::newRow("empty") << QString();
        QTest::newRow("no number") << "git version";
        QTest::newRow("no minor") << "git version 2";
        QTest::newRow("words") << "git version x.y";
        QTest::newRow("usage") << "usage: git [-v | --version] [-h | --help]";
        // Only the first line counts: a wrapper's own banner isn't git's.
        QTest::newRow("second line") << "hub version 2.14.2\ngit version 2.39.5";
    }

    void refusesWhatIsntAVersion() {
        QFETCH(QString, output);
        QVERIFY(!gitbolt::git::parseGitVersion(output));
    }

    void affected_data() {
        QTest::addColumn<QString>("output");
        QTest::addColumn<GitPlatform>("platform");
        QTest::addColumn<QStringList>("expected");

        const GitPlatform mac = GitPlatform::MacOS;
        const GitPlatform win = GitPlatform::Windows;
        const GitPlatform other = GitPlatform::Other;
        const QStringList none;
        const QStringList a{QStringLiteral("CVE-A")};
        // A line with a fix: below it, at it, past it.
        QTest::newRow("2.43.6") << "git version 2.43.6" << mac << a;
        QTest::newRow("2.43.7") << "git version 2.43.7" << mac << none;
        QTest::newRow("2.43.8") << "git version 2.43.8" << mac << none;
        // Lines without one: before, between, after the fixed ones.
        QTest::newRow("older line") << "git version 2.39.5" << mac << a;
        QTest::newRow("line between") << "git version 2.46.9" << mac << a;
        QTest::newRow("newer line") << "git version 2.51.0" << mac << none;
        // Apple: by its git version, or by Apple Git-N.
        QTest::newRow("apple 154") << "git version 2.39.5 (Apple Git-154)" << mac << a;
        QTest::newRow("apple 155") << "git version 2.39.5 (Apple Git-155)" << mac << none;
        QTest::newRow("apple fixed git") << "git version 2.50.1 (Apple Git-150)" << mac << none;
        // Git for Windows: its revision counts, and what is for upstream
        // counts for it too.
        QTest::newRow("windows rev below")
            << "git version 2.55.0.windows.3" << win << QStringList{QStringLiteral("CVE-W")};
        QTest::newRow("windows rev fixed") << "git version 2.55.0.windows.4" << win << none;
        QTest::newRow("windows 2.50.1 rev 1") << "git version 2.50.1.windows.1" << win
                                               << QStringList{QStringLiteral("CVE-W")};
        QTest::newRow("windows both") << "git version 2.45.2.windows.1" << win
                                      << QStringList{QStringLiteral("CVE-A"), QStringLiteral("CVE-W")};
        QTest::newRow("windows newer line") << "git version 2.56.0.windows.1" << win << none;
        // Only the builds an advisory names.
        QTest::newRow("windows-only, upstream build") << "git version 2.55.0" << win << none;
        QTest::newRow("someone else's") << "git version 2.45.2.vfs.0.4" << win << none;
        // Linux and the BSDs: the floor, nothing else.
        QTest::newRow("linux, affected version") << "git version 2.39.5" << other << none;
        QTest::newRow("linux, below the floor")
            << "git version 2.20.1" << other << QStringList{QStringLiteral("floor")};
        QTest::newRow("floor itself") << "git version 2.24.0" << other << none;
        QTest::newRow("mac, below the floor")
            << "git version 2.20.1" << mac << QStringList{QStringLiteral("floor"), QStringLiteral("CVE-A")};
    }

    void affected() {
        QFETCH(QString, output);
        QFETCH(GitPlatform, platform);
        QTEST(verdict(output, platform, table()), "expected");
    }

    // The table GitBolt ships reads, and says what it must about the
    // gits users have: the ones Xcode's tools and Git for Windows put
    // there. Named by ID, so a new advisory doesn't fail it.
    void builtInTableSaysWhatItShould() {
        const GitAdvisories& builtIn = gitbolt::git::builtInGitAdvisories();
        QVERIFY(!builtIn.advisories.empty());
        QVERIFY(builtIn.checked >= QDate(2026, 10, 24));
        QCOMPARE(builtIn.floor.major, 2);
        QCOMPARE(builtIn.floor.minor, 25);   // git sparse-checkout

        const auto ids = [&builtIn](const char* output, GitPlatform platform) {
            return verdict(QString::fromLatin1(output), platform, builtIn);
        };
        const QString kev = QStringLiteral("CVE-2025-48384");
        QVERIFY(ids("git version 2.39.5 (Apple Git-154)", GitPlatform::MacOS).contains(kev));
        QVERIFY(!ids("git version 2.50.1 (Apple Git-155)", GitPlatform::MacOS).contains(kev));
        QVERIFY(ids("git version 2.39.3 (Apple Git-146)", GitPlatform::MacOS)
                    .contains(QStringLiteral("CVE-2024-32002")));
        for (const char* fixed : {"git version 2.43.7", "git version 2.44.4", "git version 2.45.4",
                                  "git version 2.46.4", "git version 2.47.3", "git version 2.48.2",
                                  "git version 2.49.1", "git version 2.50.1", "git version 2.51.0"})
            QVERIFY2(!ids(fixed, GitPlatform::MacOS).contains(kev), fixed);
        QVERIFY(ids("git version 2.50.0", GitPlatform::MacOS).contains(kev));
        // Ubuntu 24.04's 2.43.0 has the fixes backported.
        QVERIFY(ids("git version 2.43.0", GitPlatform::Other).isEmpty());

        QVERIFY(ids("git version 2.53.0.windows.2", GitPlatform::Windows)
                    .contains(QStringLiteral("CVE-2026-32631")));
        QVERIFY(!ids("git version 2.53.0.windows.3", GitPlatform::Windows)
                     .contains(QStringLiteral("CVE-2026-32631")));
        // Only with transfer.bundleURI on, which is off by default: not
        // warned about.
        QVERIFY(!ids("git version 2.53.0.windows.3", GitPlatform::Windows)
                     .contains(QStringLiteral("CVE-2026-62960")));
        QVERIFY(ids("git version 2.56.0.windows.2", GitPlatform::Windows)
                    .contains(QStringLiteral("CVE-2026-105803")));
        QVERIFY(!ids("git version 2.56.0.windows.3", GitPlatform::Windows)
                     .contains(QStringLiteral("CVE-2026-105803")));
    }

    void refusesAMalformedTable_data() {
        QTest::addColumn<QByteArray>("json");
        const auto with = [](const char* advisory) {
            return QByteArray(R"({"checked": "2026-10-24", "floor": "2.24.0", "advisories": [)")
                   + advisory + "]}";
        };
        QTest::newRow("not json") << QByteArray("checked: today");
        QTest::newRow("no advisories") << with("");
        QTest::newRow("no date") << QByteArray(
            R"({"floor": "2.24.0", "advisories": [{"ids": ["X"], "risk": "code",
                "builds": ["upstream"], "fixed": ["2.1.0"]}]})");
        QTest::newRow("no ids")
            << with(R"({"ids": [], "risk": "code", "builds": ["upstream"], "fixed": ["2.1.0"]})");
        QTest::newRow("unknown risk")
            << with(R"({"ids": ["X"], "risk": "fire", "builds": ["upstream"], "fixed": ["2.1.0"]})");
        QTest::newRow("unknown build")
            << with(R"({"ids": ["X"], "risk": "code", "builds": ["linux"], "fixed": ["2.1.0"]})");
        QTest::newRow("no fixed release")
            << with(R"({"ids": ["X"], "risk": "code", "builds": ["upstream"], "fixed": []})");
        QTest::newRow("not a version")
            << with(R"({"ids": ["X"], "risk": "code", "builds": ["upstream"], "fixed": ["two"]})");
        QTest::newRow("a line twice") << with(
            R"({"ids": ["X"], "risk": "code", "builds": ["upstream"], "fixed": ["2.1.0", "2.1.3"]})");
    }

    void refusesAMalformedTable() {
        QFETCH(QByteArray, json);
        QVERIFY(!gitbolt::git::parseGitAdvisories(json));
    }
};

QTEST_GUILESS_MAIN(TestGitVersion)
#include "TestGitVersion.moc"
