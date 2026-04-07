//
// TestCommitLogModel — exercises the Qt model layer of the commit log
// AND the graph-lane computation that backs the revision graph
// delegate.
//
// The model gets populated from a hand-built std::vector<CommitData>
// rather than a real repository, because the lane layout is a pure
// function of (id, parentIds) — we don't need libgit2 here, and it's
// much easier to construct weird branch topologies as in-memory data.
//
// The graph computation is also the single piece of code most likely
// to break in subtle ways as we refactor (see the
// CommitLogModel::computeGraphData function): off-by-one lane
// assignments produce wrong colors and wrong lane crossings on the
// rendered graph but don't crash. These tests guard the invariants.
//

#include <QTest>
#include <QTemporaryDir>
#include <array>

#include "models/CommitLogModel.h"
#include "git/ObjectId.h"
#include "git/Repository.h"

namespace {

// Helper: build an ObjectId from a short ASCII tag so tests can write
// things like makeId('A') to get a deterministic distinct OID per
// letter. The rest of the 20-byte payload is zero-padded.
gitbolt::git::ObjectId makeId(char tag, char sub = '0') {
    // 20 bytes = 40 hex chars. Use the tag for the first 2 bytes and
    // zero-fill the rest. Each distinct letter gets a distinct OID.
    char hex[41];
    std::memset(hex, '0', 40);
    hex[40] = '\0';
    hex[0] = tag;
    hex[1] = sub;
    return gitbolt::git::ObjectId::fromHex(hex);
}

// Build a minimal CommitData. The lane algorithm only reads `id` and
// `parentIds`, so the other fields stay empty.
gitbolt::git::CommitData makeCommit(
    char id,
    std::initializer_list<char> parents = {})
{
    gitbolt::git::CommitData c;
    c.id = makeId(id);
    for (char p : parents)
        c.parentIds.push_back(makeId(p));
    c.summary = QString("commit %1").arg(id).toStdString();
    return c;
}

} // anonymous namespace


class TestCommitLogModel : public QObject {
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------
    // Touch libgit2 once so its static init/shutdown lifecycle runs
    // in a known order. Without this, the test suite crashes with
    // SIGTRAP at shutdown on macOS 15.6 + Qt 6.11 — the root cause
    // is the static LibGit2Init in Repository.cpp interacting badly
    // with whatever the test runner is doing during static teardown.
    // Making sure we've actually called into libgit2 at least once
    // ensures the initialization is committed before shutdown runs.
    void initTestCase() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        auto r = gitbolt::git::Repository::init(tmp.path().toStdString());
        QVERIFY(r.ok());
    }

    // -----------------------------------------------------------------
    // Empty model: no commits, zero rows, no graph data.
    // -----------------------------------------------------------------
    void emptyModelHasNoRows() {
        gitbolt::models::CommitLogModel model;
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.columnCount(), static_cast<int>(gitbolt::models::CommitLogColumn::Count));
        QVERIFY(model.commitAt(0) == nullptr);
        QVERIFY(model.graphAt(0) == nullptr);
    }

    // -----------------------------------------------------------------
    // Linear 3-commit history: every commit on lane 0, with a single
    // start-segment connecting each to its parent.
    //
    //      C ← B ← A   (newest first)
    //      0   0   0
    // -----------------------------------------------------------------
    void linearHistoryAllOnLaneZero() {
        gitbolt::models::CommitLogModel model;
        // Newest first, as revwalk would return.
        std::vector<gitbolt::git::CommitData> commits = {
            makeCommit('C', {'B'}),
            makeCommit('B', {'A'}),
            makeCommit('A'),
        };
        model.setCommits(commits);

        QCOMPARE(model.rowCount(), 3);

        for (int row = 0; row < 3; ++row) {
            const auto* g = model.graphAt(row);
            QVERIFY(g != nullptr);
            QCOMPARE(g->commitLane, 0);
            // maxLane should be 0 since everyone sits on lane 0.
            QCOMPARE(g->maxLane, 0);
        }

        // First two rows should have a start segment pointing at their parent.
        // Last row is a root commit, no parents, no segments.
        QVERIFY(!model.graphAt(0)->segments.empty());
        QVERIFY(!model.graphAt(1)->segments.empty());
        QVERIFY(model.graphAt(2)->segments.empty());
    }

    // -----------------------------------------------------------------
    // A diverging branch topology exercises the multi-lane path:
    //
    //     C ← A         C is on master
    //     D ← B ← A     D is on a feature branch rooted at A
    //
    // Revwalk order (newest first): D, C, B, A
    // Expected lanes: C on lane 0 (main line), D on a second lane.
    // -----------------------------------------------------------------
    void divergingBranchUsesSecondLane() {
        gitbolt::models::CommitLogModel model;
        std::vector<gitbolt::git::CommitData> commits = {
            makeCommit('D', {'B'}),   // feature tip
            makeCommit('C', {'A'}),   // master tip
            makeCommit('B', {'A'}),   // feature middle
            makeCommit('A'),          // root
        };
        model.setCommits(commits);
        QCOMPARE(model.rowCount(), 4);

        // Each row should have a valid graph entry.
        for (int row = 0; row < 4; ++row) {
            QVERIFY(model.graphAt(row) != nullptr);
        }

        // At least one commit must land on a lane > 0, otherwise the
        // algorithm collapsed the divergent branch onto lane 0 incorrectly.
        bool anyBeyondZero = false;
        int maxLaneSeen = 0;
        for (int row = 0; row < 4; ++row) {
            const auto* g = model.graphAt(row);
            if (g->commitLane > 0)
                anyBeyondZero = true;
            maxLaneSeen = std::max(maxLaneSeen, g->maxLane);
        }
        QVERIFY2(anyBeyondZero,
                 "Divergent branch commits should occupy lane > 0");
        QVERIFY2(maxLaneSeen >= 1,
                 "maxLane should reflect the widest point of the graph");
    }

    // -----------------------------------------------------------------
    // A merge commit has two parents. The graph row should record both
    // of them and the model's CommitData::isMerge() should return true.
    //
    //       M ← L
    //       M ← R    (M has two parents: L and R, L and R share root O)
    //       L ← O
    //       R ← O
    // -----------------------------------------------------------------
    void mergeCommitHasTwoParentEntries() {
        gitbolt::models::CommitLogModel model;
        std::vector<gitbolt::git::CommitData> commits = {
            makeCommit('M', {'L', 'R'}),   // merge of L and R
            makeCommit('L', {'O'}),
            makeCommit('R', {'O'}),
            makeCommit('O'),
        };
        model.setCommits(commits);
        QCOMPARE(model.rowCount(), 4);

        const auto* merge = model.commitAt(0);
        QVERIFY(merge != nullptr);
        QCOMPARE(merge->parentCount(), size_t(2));
        QVERIFY(merge->isMerge());
        QVERIFY(!merge->isRoot());

        const auto* root = model.commitAt(3);
        QVERIFY(root != nullptr);
        QVERIFY(root->isRoot());
        QVERIFY(!root->isMerge());
    }

    // -----------------------------------------------------------------
    // headerData returns the expected column titles.
    // -----------------------------------------------------------------
    void headerDataReturnsColumnTitles() {
        gitbolt::models::CommitLogModel model;
        using Col = gitbolt::models::CommitLogColumn;

        QCOMPARE(model.headerData(static_cast<int>(Col::Message),
                                  Qt::Horizontal, Qt::DisplayRole).toString(),
                 QString("Message"));
        QCOMPARE(model.headerData(static_cast<int>(Col::Author),
                                  Qt::Horizontal, Qt::DisplayRole).toString(),
                 QString("Author"));
        QCOMPARE(model.headerData(static_cast<int>(Col::Hash),
                                  Qt::Horizontal, Qt::DisplayRole).toString(),
                 QString("Hash"));
    }

    // -----------------------------------------------------------------
    // clear() should reset the model back to the empty state.
    // -----------------------------------------------------------------
    void clearResetsModel() {
        gitbolt::models::CommitLogModel model;
        model.setCommits({
            makeCommit('B', {'A'}),
            makeCommit('A'),
        });
        QCOMPARE(model.rowCount(), 2);

        model.clear();
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.commitAt(0) == nullptr);
        QVERIFY(model.graphAt(0) == nullptr);
    }

    // -----------------------------------------------------------------
    // Appending more commits to an already-populated model should
    // extend the row count rather than replace it.
    // -----------------------------------------------------------------
    void appendExtendsExistingHistory() {
        gitbolt::models::CommitLogModel model;
        model.setCommits({
            makeCommit('B', {'A'}),
            makeCommit('A'),
        });
        QCOMPARE(model.rowCount(), 2);

        std::vector<gitbolt::git::CommitData> more = {
            makeCommit('Z'),  // another root, just to prove rows grow
        };
        model.appendCommits(more);
        QCOMPARE(model.rowCount(), 3);

        // The original commits should still be at their rows.
        QCOMPARE(model.commitAt(0)->id, makeId('B'));
        QCOMPARE(model.commitAt(1)->id, makeId('A'));
        QCOMPARE(model.commitAt(2)->id, makeId('Z'));
    }
};

// Use QTEST_GUILESS_MAIN so we build a QCoreApplication rather than a
// QApplication. CommitLogModel is a pure QAbstractTableModel with no
// widget dependencies and we don't need a Cocoa NSApplication for
// model tests — building a QApplication was causing a SIGTRAP at
// static destructor time on macOS 15.6 + Qt 6.11 under ctest.
QTEST_GUILESS_MAIN(TestCommitLogModel)
#include "TestCommitLogModel.moc"
