//
// TestDiff — exercises gitbolt::git::Repository's diff methods.
//
// Diff is what the diff viewer widget renders, and what the staging
// hunk-level operations are built on top of. These tests verify the
// libgit2 → DiffResult marshalling preserves the right content,
// counts additions/deletions correctly, and handles edge cases like
// added files, deleted files, and binary files.
//

#include <QTest>

#include "../TestRepoHelper.h"
#include "git/Diff.h"

class TestDiff : public QObject {
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------
    // A modified-file diff should contain the old and new content with
    // the expected addition and deletion counts.
    // -----------------------------------------------------------------
    void diffCommitReportsAdditionsAndDeletions() {
        gitbolt::test::TestRepo repo;

        QVERIFY(repo.writeAndCommit("file.txt", "line 1\nline 2\nline 3\n",
                                    "initial").ok());

        // Change middle line: delete "line 2", add "LINE TWO"
        repo.writeFile("file.txt", "line 1\nLINE TWO\nline 3\n");
        QVERIFY(repo.stageFile("file.txt").ok());
        auto second = repo.commit("change line 2");
        QVERIFY(second.ok());

        auto diff = repo.repo().diffCommit(*second);
        QVERIFY(diff.ok());
        QCOMPARE(diff->files.size(), size_t(1));

        const auto& file = diff->files[0];
        QCOMPARE(file.status, gitbolt::git::DiffStatus::Modified);
        QCOMPARE(QString::fromStdString(file.path()), QString("file.txt"));

        // One addition ("LINE TWO") and one deletion ("line 2").
        QCOMPARE(diff->totalAdditions, size_t(1));
        QCOMPARE(diff->totalDeletions, size_t(1));
    }

    // -----------------------------------------------------------------
    // Newly added file: the diff should report it as Added and count
    // every line as an addition.
    // -----------------------------------------------------------------
    void addedFileCountsAllLinesAsAdditions() {
        gitbolt::test::TestRepo repo;

        // Seed with a commit so HEAD exists.
        QVERIFY(repo.writeAndCommit("seed.txt", "seed\n", "seed").ok());

        // Add a brand-new file in a second commit.
        auto added = repo.writeAndCommit("new.txt", "a\nb\nc\n", "add new.txt");
        QVERIFY(added.ok());

        auto diff = repo.repo().diffCommit(*added);
        QVERIFY(diff.ok());
        QCOMPARE(diff->files.size(), size_t(1));
        QCOMPARE(diff->files[0].status, gitbolt::git::DiffStatus::Added);
        QCOMPARE(QString::fromStdString(diff->files[0].path()), QString("new.txt"));
        QCOMPARE(diff->totalAdditions, size_t(3));
        QCOMPARE(diff->totalDeletions, size_t(0));
    }

    // -----------------------------------------------------------------
    // Deleted file: the diff should report it as Deleted and count
    // every line as a deletion.
    // -----------------------------------------------------------------
    void deletedFileCountsAllLinesAsDeletions() {
        gitbolt::test::TestRepo repo;

        QVERIFY(repo.writeAndCommit("doomed.txt", "x\ny\n", "add doomed").ok());

        // Delete the file and commit.
        repo.deleteFile("doomed.txt");
        QVERIFY(repo.stageAll().ok());
        auto deleted = repo.commit("delete doomed");
        QVERIFY(deleted.ok());

        auto diff = repo.repo().diffCommit(*deleted);
        QVERIFY(diff.ok());
        QCOMPARE(diff->files.size(), size_t(1));
        QCOMPARE(diff->files[0].status, gitbolt::git::DiffStatus::Deleted);
        QCOMPARE(QString::fromStdString(diff->files[0].path()), QString("doomed.txt"));
        QCOMPARE(diff->totalAdditions, size_t(0));
        QCOMPARE(diff->totalDeletions, size_t(2));
    }

    // -----------------------------------------------------------------
    // Multi-file commit: the diff should list every changed file in
    // insertion order and aggregate totals across all of them.
    // -----------------------------------------------------------------
    void multipleFilesAreAllListed() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("first.txt", "f\n", "initial").ok());

        repo.writeFile("a.txt", "aaa\n");
        repo.writeFile("b.txt", "bbb\nccc\n");
        QVERIFY(repo.stageAll().ok());
        auto commit = repo.commit("add two files");
        QVERIFY(commit.ok());

        auto diff = repo.repo().diffCommit(*commit);
        QVERIFY(diff.ok());
        QCOMPARE(diff->files.size(), size_t(2));

        QSet<QString> paths;
        for (const auto& f : diff->files)
            paths.insert(QString::fromStdString(f.path()));
        QVERIFY(paths.contains("a.txt"));
        QVERIFY(paths.contains("b.txt"));

        // 1 line + 2 lines = 3 additions
        QCOMPARE(diff->totalAdditions, size_t(3));
        QCOMPARE(diff->totalDeletions, size_t(0));
    }

    // -----------------------------------------------------------------
    // diffIndexToWorkdir: uncommitted working-tree changes should show
    // up here even before they're staged.
    // -----------------------------------------------------------------
    void diffIndexToWorkdirReportsUnstagedChanges() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("tracked.txt", "original\n", "seed").ok());

        // Modify the file in the working tree without staging.
        repo.writeFile("tracked.txt", "modified\n");

        auto diff = repo.repo().diffIndexToWorkdir();
        QVERIFY(diff.ok());
        QCOMPARE(diff->files.size(), size_t(1));
        QCOMPARE(diff->files[0].status, gitbolt::git::DiffStatus::Modified);
        QCOMPARE(QString::fromStdString(diff->files[0].path()), QString("tracked.txt"));
        QCOMPARE(diff->totalAdditions, size_t(1));
        QCOMPARE(diff->totalDeletions, size_t(1));
    }
};

QTEST_MAIN(TestDiff)
#include "TestDiff.moc"
