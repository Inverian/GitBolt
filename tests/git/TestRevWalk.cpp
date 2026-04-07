//
// TestRevWalk — exercises gitbolt::git::RevWalk against a temporary
// repository built via TestRepoHelper.
//
// RevWalk is the backbone of the revision graph: if this is broken,
// the entire commit history view shows nothing (or shows the wrong
// things in the wrong order). These tests guard against regressions
// in the wrapper's lifetime, sorting, and iteration semantics.
//

#include <QTest>

#include "../TestRepoHelper.h"
#include "git/Revwalk.h"

class TestRevWalk : public QObject {
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------
    // Happy-path: walk a single linear history and verify that
    // topological order is honored (child commits appear before
    // their parents).
    //
    // Note: all three commits may share the same wall-clock second
    // during a fast test run, so we rely on topological sort for
    // order rather than timestamps. We use `TopologicalTime` which
    // prefers time but falls back to topology for ties.
    // -----------------------------------------------------------------
    void linearHistoryReturnedNewestFirst() {
        gitbolt::test::TestRepo repo;

        // Build three commits: A → B → C (A is root, C is tip)
        auto a = repo.writeAndCommit("a.txt", "a\n", "commit A");
        QVERIFY(a.ok());
        auto b = repo.writeAndCommit("b.txt", "b\n", "commit B");
        QVERIFY(b.ok());
        auto c = repo.writeAndCommit("c.txt", "c\n", "commit C");
        QVERIFY(c.ok());

        auto walkResult = repo.repo().createRevWalk();
        QVERIFY(walkResult.ok());
        auto& walk = *walkResult;

        // Topological sort guarantees: a commit is emitted before
        // any of its ancestors. The actual emission order of two
        // unrelated commits is unspecified — but our history is
        // strictly linear so the order is fully determined.
        walk.setSorting(gitbolt::git::SortOrder::Topological);
        QVERIFY(walk.pushHead().ok());

        auto commits = walk.all();
        QVERIFY(commits.ok());
        QCOMPARE(commits->size(), size_t(3));

        // Topological order for a linear chain: tip first, root last.
        QCOMPARE((*commits)[0].id, *c);
        QCOMPARE((*commits)[1].id, *b);
        QCOMPARE((*commits)[2].id, *a);
    }

    // -----------------------------------------------------------------
    // Paged iteration: next(N) should return exactly N commits (or
    // fewer on the last page) and never re-yield the same commit.
    // -----------------------------------------------------------------
    void nextReturnsRequestedPageSize() {
        gitbolt::test::TestRepo repo;

        // Five commits
        for (int i = 1; i <= 5; ++i) {
            const auto msg = QStringLiteral("commit %1").arg(i);
            const auto rel = QStringLiteral("file%1.txt").arg(i);
            auto r = repo.writeAndCommit(rel, "x\n", msg);
            QVERIFY(r.ok());
        }

        auto walk = repo.repo().createRevWalk();
        QVERIFY(walk.ok());
        walk->setSorting(gitbolt::git::SortOrder::Topological);
        QVERIFY(walk->pushHead().ok());

        auto page1 = walk->next(2);
        QVERIFY(page1.ok());
        QCOMPARE(page1->size(), size_t(2));

        auto page2 = walk->next(2);
        QVERIFY(page2.ok());
        QCOMPARE(page2->size(), size_t(2));

        auto page3 = walk->next(2);
        QVERIFY(page3.ok());
        QCOMPARE(page3->size(), size_t(1));  // only 1 commit left

        auto page4 = walk->next(2);
        QVERIFY(page4.ok());
        QCOMPARE(page4->size(), size_t(0));  // exhausted

        // Pages should contain distinct commits.
        QVERIFY((*page1)[0].id != (*page2)[0].id);
        QVERIFY((*page1)[0].id != (*page3)[0].id);
        QVERIFY((*page2)[0].id != (*page3)[0].id);
    }

    // -----------------------------------------------------------------
    // reset() restarts iteration from scratch.
    // -----------------------------------------------------------------
    void resetRewindsIteration() {
        gitbolt::test::TestRepo repo;

        auto c1 = repo.writeAndCommit("a.txt", "a\n", "commit 1");
        QVERIFY(c1.ok());
        auto c2 = repo.writeAndCommit("b.txt", "b\n", "commit 2");
        QVERIFY(c2.ok());

        auto walk = repo.repo().createRevWalk();
        QVERIFY(walk.ok());
        walk->setSorting(gitbolt::git::SortOrder::Topological);
        QVERIFY(walk->pushHead().ok());

        auto firstPass = walk->all();
        QVERIFY(firstPass.ok());
        QCOMPARE(firstPass->size(), size_t(2));

        // After all() the iterator is exhausted.
        auto exhausted = walk->next(1);
        QVERIFY(exhausted.ok());
        QCOMPARE(exhausted->size(), size_t(0));

        // reset + re-push should give us the same commits again.
        walk->reset();
        QVERIFY(walk->pushHead().ok());
        auto secondPass = walk->all();
        QVERIFY(secondPass.ok());
        QCOMPARE(secondPass->size(), size_t(2));
        QCOMPARE((*secondPass)[0].id, (*firstPass)[0].id);
        QCOMPARE((*secondPass)[1].id, (*firstPass)[1].id);
    }

    // -----------------------------------------------------------------
    // Parent/child linkage: each commit data should list its parents
    // with the correct OID.
    // -----------------------------------------------------------------
    void commitDataReportsParents() {
        gitbolt::test::TestRepo repo;

        auto a = repo.writeAndCommit("a.txt", "a\n", "commit A");
        QVERIFY(a.ok());
        auto b = repo.writeAndCommit("b.txt", "b\n", "commit B");
        QVERIFY(b.ok());

        auto walk = repo.repo().createRevWalk();
        QVERIFY(walk.ok());
        walk->setSorting(gitbolt::git::SortOrder::Topological);
        QVERIFY(walk->pushHead().ok());
        auto commits = walk->all();
        QVERIFY(commits.ok());
        QCOMPARE(commits->size(), size_t(2));

        const auto& newest = (*commits)[0];  // commit B
        const auto& oldest = (*commits)[1];  // commit A

        QCOMPARE(newest.parentCount(), size_t(1));
        QCOMPARE(newest.parentIds[0], *a);      // B's parent is A
        QCOMPARE(oldest.parentCount(), size_t(0));  // A is root
        QVERIFY(oldest.isRoot());
        QVERIFY(!newest.isRoot());
        QVERIFY(!newest.isMerge());
    }

    // -----------------------------------------------------------------
    // Walking an empty (unborn HEAD) repo should not crash and should
    // report an error or empty result when we try to push HEAD.
    // -----------------------------------------------------------------
    void emptyRepoDoesNotCrash() {
        gitbolt::test::TestRepo repo;  // no commits

        auto walk = repo.repo().createRevWalk();
        QVERIFY(walk.ok());

        // Pushing HEAD on an unborn branch should fail cleanly (not crash).
        auto pushResult = walk->pushHead();
        QVERIFY(!pushResult.ok());
    }
};

QTEST_MAIN(TestRevWalk)
#include "TestRevWalk.moc"
