// Tests for probeRecentRepo: the welcome screen's Branch cell.

#include <QTest>

#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "services/RecentRepoProbe.h"

using gitbolt::services::probeRecentRepo;

namespace {
bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return r.ok() && r.value().success();
}
} // namespace

class TestRecentRepoProbe : public QObject {
    Q_OBJECT

private slots:
    // A new repository's branch, before its first commit: the cell was
    // blank while the status bar named it.
    void aNewRepositoryNamesItsBranch() {
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"symbolic-ref", "HEAD", "refs/heads/trunk"})));

        const auto info = probeRecentRepo(repo.path());
        QVERIFY(info.valid);
        QCOMPARE(info.branch, QStringLiteral("trunk"));
        QVERIFY(!info.detachedHead);
    }

    void aBranchAndADetachedHead() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "topic"})));
        QCOMPARE(probeRecentRepo(repo.path()).branch, QStringLiteral("topic"));

        QVERIFY(succeeded(git.run({"checkout", "-q", "--detach"})));
        const auto info = probeRecentRepo(repo.path());
        QVERIFY(info.detachedHead);
        QVERIFY(info.branch.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRecentRepoProbe)
#include "TestRecentRepoProbe.moc"
