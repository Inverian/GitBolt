//
// TestBranchModel — the sidebar's refs tree, fed from a real
// repository so the model sees exactly what Repository hands it.
//
// Pins the naming contract for tag rows: the tree shows short names
// ("v1", not "refs/tags/v1"), while a checkout from a tag row hands
// Repository::checkout the full ref, so a local branch that shares
// the tag's name can't capture it. Also pins what an annotated tag's
// row reports: the tagged commit, the tagger and the message.
//

#include <QTest>

#include "../TestRepoHelper.h"
#include "models/BranchModel.h"

using gitbolt::models::BranchModel;

class TestBranchModel : public QObject {
    Q_OBJECT

private slots:
    void tagRowsShowShortNamesAndCheckOutFullRef() {
        gitbolt::test::TestRepo repo;
        auto tagged = repo.writeAndCommit("a.txt", "a\n", "tagged");
        QVERIFY(tagged.ok());
        QVERIFY(repo.repo().createTag("v1", tagged.value(), "release").ok());
        auto tip = repo.writeAndCommit("b.txt", "b\n", "tip");
        QVERIFY(tip.ok());
        // Same name as the tag, different commit.
        QVERIFY(repo.repo().createBranch("v1", tip.value()).ok());

        BranchModel model;
        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        model.setTags(std::move(tags.value()));
        auto branches = repo.repo().allBranches();
        QVERIFY(branches.ok());
        model.setBranches(std::move(branches.value()));

        const QModelIndex tagsRoot = model.index(
            static_cast<int>(BranchModel::RootCategory::Tags), 0);
        QCOMPARE(model.rowCount(tagsRoot), 1);
        const QModelIndex tagRow = model.index(0, 0, tagsRoot);
        QCOMPARE(model.data(tagRow).toString(), QStringLiteral("v1"));
        QCOMPARE(model.branchNameAt(tagRow), QStringLiteral("v1"));
        QCOMPARE(model.data(tagRow, BranchModel::FullRefNameRole).toString(),
                 QStringLiteral("refs/tags/v1"));
        QCOMPARE(model.checkoutRefAt(tagRow), QStringLiteral("refs/tags/v1"));

        // Branch rows keep the short name: checkout only attaches HEAD
        // for a bare local branch name.
        const QModelIndex localRoot = model.index(
            static_cast<int>(BranchModel::RootCategory::LocalBranches), 0);
        bool sawBranch = false;
        for (int row = 0; row < model.rowCount(localRoot); ++row) {
            const QModelIndex idx = model.index(row, 0, localRoot);
            if (model.branchNameAt(idx) == QStringLiteral("v1")) {
                QCOMPARE(model.checkoutRefAt(idx), QStringLiteral("v1"));
                sawBranch = true;
            }
        }
        QVERIFY(sawBranch);

        // The tag row's checkout lands on the tag, detached — not on
        // the branch.
        QVERIFY(repo.repo().checkout(
            model.checkoutRefAt(tagRow).toStdString()).ok());
        QVERIFY(repo.repo().isHeadDetached());
        auto head = repo.repo().head();
        QVERIFY(head.ok());
        QCOMPARE(head.value(), tagged.value());
    }

    // An annotated tag's row reports the commit it tags (not the tag
    // object's id), who tagged it, and its message.
    void annotatedTagRowShowsCommitTaggerAndMessage() {
        gitbolt::test::TestRepo repo;
        auto tagged = repo.writeAndCommit("a.txt", "a\n", "tagged");
        QVERIFY(tagged.ok());
        QVERIFY(repo.repo().createTag("v1", tagged.value(), "release notes").ok());
        QVERIFY(repo.writeAndCommit("b.txt", "b\n", "tip").ok());

        BranchModel model;
        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        model.setTags(std::move(tags.value()));

        const QModelIndex tagsRoot = model.index(
            static_cast<int>(BranchModel::RootCategory::Tags), 0);
        QCOMPARE(model.rowCount(tagsRoot), 1);
        const QModelIndex tagRow = model.index(0, 0, tagsRoot);
        QCOMPARE(model.data(tagRow, BranchModel::ObjectIdRole).toString(),
                 QString::fromStdString(tagged->toHex()));
        const QString tip = model.data(tagRow, Qt::ToolTipRole).toString();
        QVERIFY2(tip.contains(QString::fromStdString(tagged->toShortHex())),
                 qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("Test User <test@gitbolt.local>")),
                 qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("release notes")), qPrintable(tip));
    }
};

QTEST_MAIN(TestBranchModel)
#include "TestBranchModel.moc"
