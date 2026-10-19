//
// TestBranchModel — the sidebar's refs tree, fed from a real
// repository so the model sees exactly what Repository hands it.
//
// Pins the naming contract for tag rows: the tree shows short names
// ("v1", not "refs/tags/v1"), while a checkout from a tag row hands
// Repository::checkout the full ref, so a local branch that shares
// the tag's name can't capture it. Also pins what an annotated tag's
// row reports: the tagged commit, the tagger and the message, and that
// every tooltip shows the repository's text as written, markup and all
// (gitbolt::test::shownToolTip: what the tooltip puts on the screen).
//

#include <QTest>

#include "../ShownText.h"
#include "../TestRepoHelper.h"
#include "models/BranchModel.h"

using gitbolt::models::BranchModel;
using gitbolt::test::asName;
using gitbolt::test::shownToolTip;

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
        const QString tip = shownToolTip(model.data(tagRow, Qt::ToolTipRole).toString());
        QVERIFY2(tip.contains(QString::fromStdString(tagged->toShortHex())),
                 qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("Test User <test@gitbolt.local>")),
                 qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("release notes")), qPrintable(tip));
    }

    // A signed tag's row shows its message, not the signature block
    // `git tag -s` appends to it (createTag writes these bytes as-is).
    void signedTagRowHidesTheSignature() {
        gitbolt::test::TestRepo repo;
        auto tagged = repo.writeAndCommit("a.txt", "a\n", "tagged");
        QVERIFY(tagged.ok());
        QVERIFY(repo.repo().createTag("v1", tagged.value(),
            "release notes\n"
            "-----BEGIN SSH SIGNATURE-----\n"
            "U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTk=\n"
            "-----END SSH SIGNATURE-----\n").ok());

        BranchModel model;
        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        model.setTags(std::move(tags.value()));

        const QModelIndex tagsRoot = model.index(
            static_cast<int>(BranchModel::RootCategory::Tags), 0);
        QCOMPARE(model.rowCount(tagsRoot), 1);
        const QString tip = shownToolTip(model.data(model.index(0, 0, tagsRoot),
                                                    Qt::ToolTipRole).toString());
        QVERIFY2(tip.endsWith(QStringLiteral("\nrelease notes")), qPrintable(tip));
        QVERIFY2(!tip.contains(QStringLiteral("BEGIN")), qPrintable(tip));
        QVERIFY2(!tip.contains(QStringLiteral("SIGNATURE")), qPrintable(tip));
    }

    // A ref's name, a tag's message, a submodule's name and URL and a
    // stash's message are the repository's to write, a clone's from the
    // server: each tooltip shows them as written. Qt took them for rich
    // text: a branch "<b>MARKER</b>" (a valid name) showed as a bold
    // MARKER, and a tag's message could hide its lines or draw a link.
    // Line breaks and indentation stay. (The branch and the tag on disk
    // are named with "&lt;" on Windows: gitbolt::test::asName.)
    void tooltipsShowTheRepositorysTextAsWritten() {
        const QString bold = QStringLiteral("<b>MARKER</b>");
        const QString link = QStringLiteral("<a href=\"x-test:1\">L</a>");
        const QString branch = asName(bold);
        gitbolt::test::TestRepo repo;
        auto tip = repo.writeAndCommit("a.txt", "a\n", "tip");
        QVERIFY(tip.ok());
        QVERIFY(repo.repo().createBranch(branch.toStdString(), tip.value()).ok());
        const QString tag = asName(QStringLiteral("<i>v1</i>"));
        const QString message = link + QStringLiteral("\n    indented & <i>kept</i>");
        QVERIFY(repo.repo().createTag(tag.toStdString(), tip.value(),
                                      message.toStdString()).ok());

        BranchModel model;
        auto branches = repo.repo().allBranches();
        QVERIFY(branches.ok());
        // And a remote-tracking branch of that name, as a fetch makes.
        gitbolt::git::BranchInfo remote;
        remote.name = "origin/" + bold.toStdString();
        remote.fullRefName = "refs/remotes/origin/" + bold.toStdString();
        remote.tipId = tip.value();
        remote.type = gitbolt::git::BranchType::Remote;
        branches.value().push_back(remote);
        model.setBranches(std::move(branches.value()));
        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        model.setTags(std::move(tags.value()));
        gitbolt::git::SubmoduleInfo sub;
        sub.name = bold.toStdString();
        sub.url = link.toStdString();
        sub.status = gitbolt::git::SubmoduleStatus::Clean;
        model.setSubmodules({sub});
        gitbolt::git::StashEntry stash;
        stash.index = 0;
        stash.message = "On main: " + bold.toStdString();
        model.setStashes({stash});

        const auto rootOf = [&](BranchModel::RootCategory category) {
            return model.index(static_cast<int>(category), 0);
        };
        const auto tipAt = [&](const QModelIndex& row) {
            return shownToolTip(model.data(row, Qt::ToolTipRole).toString());
        };

        const QModelIndex localRoot = rootOf(BranchModel::RootCategory::LocalBranches);
        QModelIndex branchRow;
        for (int row = 0; row < model.rowCount(localRoot); ++row) {
            if (model.branchNameAt(model.index(row, 0, localRoot)) == branch)
                branchRow = model.index(row, 0, localRoot);
        }
        QVERIFY(branchRow.isValid());
        QCOMPARE(tipAt(branchRow), QStringLiteral("refs/heads/") + branch);
        QCOMPARE(tipAt(model.index(0, 0, rootOf(BranchModel::RootCategory::RemoteBranches))),
                 QStringLiteral("refs/remotes/origin/") + bold);

        const QModelIndex tagRow = model.index(0, 0, rootOf(BranchModel::RootCategory::Tags));
        QCOMPARE(tipAt(tagRow),
                 QStringLiteral("refs/tags/%1\nTarget: %2\nTagger: Test User "
                                "<test@gitbolt.local>\n%3")
                     .arg(tag, QString::fromStdString(tip->toShortHex()), message));

        QCOMPARE(tipAt(model.index(0, 0, rootOf(BranchModel::RootCategory::Submodules))),
                 bold + QStringLiteral("\nURL: ") + link);
        QCOMPARE(tipAt(model.index(0, 0, rootOf(BranchModel::RootCategory::Stashes))),
                 QStringLiteral("stash@{0}\nOn main: ") + bold);
    }
};

QTEST_MAIN(TestBranchModel)
#include "TestBranchModel.moc"
