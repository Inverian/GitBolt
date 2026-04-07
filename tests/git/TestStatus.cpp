//
// TestStatus — exercises gitbolt::git::Repository::status() and the
// StatusEntry convenience methods (isStaged, isWorkingTree, isUntracked).
//
// The staging widget's entire display depends on these being correct.
// A regression here would show wrong file counts or miscategorize
// staged-vs-unstaged changes.
//

#include <QTest>

#include "../TestRepoHelper.h"
#include "git/Status.h"

class TestStatus : public QObject {
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------
    // A freshly initialised repo with one committed file and no
    // pending changes should report an empty status list.
    // -----------------------------------------------------------------
    void cleanRepoReportsNoStatusEntries() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("file.txt", "clean\n", "initial").ok());

        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(0));
    }

    // -----------------------------------------------------------------
    // A new untracked file should show up with the WtNew flag and
    // be recognized as untracked and working-tree-only.
    // -----------------------------------------------------------------
    void untrackedFileIsReported() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("seed.txt", "seed\n", "seed").ok());

        repo.writeFile("new.txt", "untracked\n");

        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(1));

        const auto& entry = (*status)[0];
        QCOMPARE(QString::fromStdString(entry.path), QString("new.txt"));
        QVERIFY(entry.isUntracked());
        QVERIFY(entry.isWorkingTree());
        QVERIFY(!entry.isStaged());
        QVERIFY(!entry.isConflicted());
    }

    // -----------------------------------------------------------------
    // Modifying a tracked file without staging produces a single
    // WtModified entry.
    // -----------------------------------------------------------------
    void modifiedUnstagedFile() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("tracked.txt", "v1\n", "initial").ok());

        repo.writeFile("tracked.txt", "v2\n");

        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(1));

        const auto& e = (*status)[0];
        QCOMPARE(QString::fromStdString(e.path), QString("tracked.txt"));
        QVERIFY(e.isWorkingTree());
        QVERIFY(!e.isStaged());
        QVERIFY(!e.isUntracked());  // it's tracked, just modified
        QVERIFY(hasFlag(e.status, gitbolt::git::FileStatus::WtModified));
    }

    // -----------------------------------------------------------------
    // Staging a new file turns it into an IndexNew entry — the staged
    // filter should pick it up.
    // -----------------------------------------------------------------
    void stagedNewFile() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("seed.txt", "seed\n", "seed").ok());

        repo.writeFile("brand_new.txt", "hello\n");
        QVERIFY(repo.stageFile("brand_new.txt").ok());

        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(1));

        const auto& e = (*status)[0];
        QCOMPARE(QString::fromStdString(e.path), QString("brand_new.txt"));
        QVERIFY(e.isStaged());
        QVERIFY(!e.isUntracked());
        QVERIFY(hasFlag(e.status, gitbolt::git::FileStatus::IndexNew));
    }

    // -----------------------------------------------------------------
    // The same file can be both staged AND have further unstaged
    // changes at the same time (stage v2, then edit to v3). Both
    // flags should be set on the single entry.
    // -----------------------------------------------------------------
    void stagedAndFurtherUnstagedOnSameFile() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("tracked.txt", "v1\n", "initial").ok());

        // Stage v2
        repo.writeFile("tracked.txt", "v2\n");
        QVERIFY(repo.stageFile("tracked.txt").ok());

        // Then edit further to v3 without staging
        repo.writeFile("tracked.txt", "v3\n");

        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(1));

        const auto& e = (*status)[0];
        QVERIFY(e.isStaged());        // v1 → v2 is staged
        QVERIFY(e.isWorkingTree());   // v2 → v3 is unstaged
        QVERIFY(hasFlag(e.status, gitbolt::git::FileStatus::IndexModified));
        QVERIFY(hasFlag(e.status, gitbolt::git::FileStatus::WtModified));
    }

    // -----------------------------------------------------------------
    // Multiple files in multiple states — the status list should
    // contain one entry per file and each should have the right flags.
    // -----------------------------------------------------------------
    void multipleFilesInDifferentStates() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "initial a").ok());
        QVERIFY(repo.writeAndCommit("b.txt", "b\n", "initial b").ok());

        // a.txt: modified (unstaged)
        repo.writeFile("a.txt", "modified\n");
        // b.txt: modified and staged
        repo.writeFile("b.txt", "staged\n");
        QVERIFY(repo.stageFile("b.txt").ok());
        // c.txt: new untracked
        repo.writeFile("c.txt", "untracked\n");

        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(3));

        // Build a map from path → entry to avoid relying on order.
        QHash<QString, const gitbolt::git::StatusEntry*> byPath;
        for (const auto& e : *status)
            byPath.insert(QString::fromStdString(e.path), &e);

        QVERIFY(byPath.contains("a.txt"));
        QVERIFY(byPath.contains("b.txt"));
        QVERIFY(byPath.contains("c.txt"));

        QVERIFY(byPath["a.txt"]->isWorkingTree());
        QVERIFY(!byPath["a.txt"]->isStaged());

        QVERIFY(byPath["b.txt"]->isStaged());
        QVERIFY(!byPath["b.txt"]->isWorkingTree());

        QVERIFY(byPath["c.txt"]->isUntracked());
    }

    // -----------------------------------------------------------------
    // Sanity test for the FileStatus bitflag helpers: OR-ing two flags
    // and hasFlag both work as expected.
    // -----------------------------------------------------------------
    void fileStatusBitflagsCompose() {
        using FS = gitbolt::git::FileStatus;
        const auto combined = FS::IndexModified | FS::WtModified;
        QVERIFY(hasFlag(combined, FS::IndexModified));
        QVERIFY(hasFlag(combined, FS::WtModified));
        QVERIFY(!hasFlag(combined, FS::IndexNew));
        QVERIFY(!hasFlag(combined, FS::WtDeleted));
    }
};

QTEST_MAIN(TestStatus)
#include "TestStatus.moc"
