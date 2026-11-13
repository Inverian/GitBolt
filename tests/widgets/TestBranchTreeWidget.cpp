// Tests for the sidebar's refs tree across reloads: expanded and hidden
// categories, selection and scroll stay; another repository starts afresh.

#include <QAbstractItemModel>
#include <QLineEdit>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QTreeView>

#include "models/BranchModel.h"
#include "widgets/BranchTreeWidget.h"

using gitbolt::git::BranchInfo;
using gitbolt::git::BranchType;
using gitbolt::git::TagInfo;
using gitbolt::models::BranchModel;
using gitbolt::widgets::BranchTreeWidget;

namespace {

BranchInfo local(const std::string& name, bool head = false) {
    BranchInfo b;
    b.name = name;
    b.fullRefName = "refs/heads/" + name;
    b.type = BranchType::Local;
    b.isHead = head;
    return b;
}

BranchInfo remote(const std::string& name) {
    BranchInfo b;
    b.name = name;
    b.fullRefName = "refs/remotes/" + name;
    b.type = BranchType::Remote;
    return b;
}

std::vector<TagInfo> tags(const std::vector<std::string>& names) {
    std::vector<TagInfo> list;
    for (const std::string& name : names) {
        TagInfo t;
        t.name = name;
        t.fullRefName = "refs/tags/" + name;
        list.push_back(t);
    }
    return list;
}

// "v0.1.000" … in order, `count` of them.
std::vector<std::string> numbered(const char* prefix, int count) {
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i)
        names.push_back(QStringLiteral("%1%2").arg(QLatin1String(prefix)).arg(i, 3, 10, QChar('0'))
                            .toStdString());
    return names;
}

QString keyOf(const QModelIndex& index) {
    return index.data(BranchModel::ItemKeyRole).toString();
}

// The view's row of `key`, at any depth.
QModelIndex rowOf(const QTreeView* view, const QString& key, const QModelIndex& parent = {}) {
    const QAbstractItemModel* model = view->model();
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (keyOf(index) == key)
            return index;
        if (const QModelIndex below = rowOf(view, key, index); below.isValid())
            return below;
    }
    return {};
}

QModelIndex category(const QTreeView* view, BranchModel::RootCategory cat) {
    return rowOf(view, QStringLiteral("category:%1").arg(static_cast<int>(cat)));
}

bool expanded(const QTreeView* view, BranchModel::RootCategory cat) {
    return view->isExpanded(category(view, cat));
}

struct Sidebar {
    BranchTreeWidget tree;
    QTreeView* view = nullptr;

    Sidebar() {
        view = tree.findChild<QTreeView*>(QStringLiteral("branchTree.view"));
        tree.resize(320, 240);
        tree.show();
    }
};

} // namespace

class TestBranchTreeWidget : public QObject {
    Q_OBJECT

private slots:
    // Remote Branches and Tags opened, Local Branches closed by the user
    // and a tag selected: a branch made outside, then a tag sorted above
    // the selected one, change none of it.
    void aReloadKeepsTheExpandedCategoriesAndTheSelection() {
        Sidebar s;
        QVERIFY(s.view && QTest::qWaitForWindowExposed(&s.tree));
        s.tree.setBranches({local("main", true), local("feature"), remote("origin/main")});
        s.tree.setTags(tags({"v0.1.0", "v0.1.1"}));
        QVERIFY(expanded(s.view, BranchModel::RootCategory::LocalBranches));
        QVERIFY(!expanded(s.view, BranchModel::RootCategory::Tags));

        s.view->collapse(category(s.view, BranchModel::RootCategory::LocalBranches));
        s.view->expand(category(s.view, BranchModel::RootCategory::RemoteBranches));
        s.view->expand(category(s.view, BranchModel::RootCategory::Tags));
        s.view->setCurrentIndex(rowOf(s.view, QStringLiteral("refs/tags/v0.1.1")));

        QSignalSpy resets(s.tree.branchModel(), &QAbstractItemModel::modelReset);
        s.tree.setBranches({local("main", true), local("feature"), local("other"),
                            remote("origin/main"), remote("origin/other")});
        QCOMPARE(resets.count(), 1);
        QVERIFY(rowOf(s.view, QStringLiteral("refs/heads/other")).isValid());
        QVERIFY(!expanded(s.view, BranchModel::RootCategory::LocalBranches));
        QVERIFY(expanded(s.view, BranchModel::RootCategory::RemoteBranches));
        QVERIFY(expanded(s.view, BranchModel::RootCategory::Tags));
        QCOMPARE(keyOf(s.view->currentIndex()), QStringLiteral("refs/tags/v0.1.1"));
        QCOMPARE(s.view->selectionModel()->selectedIndexes().size(), 1);
        QCOMPARE(keyOf(s.view->selectionModel()->selectedIndexes().first()),
                 QStringLiteral("refs/tags/v0.1.1"));

        // The selected tag moves down a row; it stays the selected one.
        s.tree.setTags(tags({"v0.0.9", "v0.1.0", "v0.1.1"}));
        QCOMPARE(resets.count(), 2);
        QCOMPARE(s.view->currentIndex().row(), 2);
        QCOMPARE(keyOf(s.view->selectionModel()->selectedIndexes().value(0)),
                 QStringLiteral("refs/tags/v0.1.1"));
        QVERIFY(expanded(s.view, BranchModel::RootCategory::Tags));

        // Gone, it takes the selection with it.
        s.tree.setTags(tags({"v0.0.9", "v0.1.0"}));
        QVERIFY(!s.view->currentIndex().isValid());
        QVERIFY(s.view->selectionModel()->selectedIndexes().isEmpty());
        QVERIFY(expanded(s.view, BranchModel::RootCategory::Tags));
    }

    // The row at the top of the view stays at the top, rows added above
    // it or not: the view doesn't jump at each reload.
    void theRowAtTheTopStaysThere() {
        Sidebar s;
        QVERIFY(s.view && QTest::qWaitForWindowExposed(&s.tree));
        s.tree.setBranches({local("main", true)});
        s.tree.setTags(tags(numbered("v0.1.", 200)));
        s.view->expand(category(s.view, BranchModel::RootCategory::Tags));
        s.view->scrollTo(rowOf(s.view, QStringLiteral("refs/tags/v0.1.100")),
                         QAbstractItemView::PositionAtTop);
        const QModelIndex top = s.view->indexAt(QPoint(0, 0));
        QCOMPARE(keyOf(top), QStringLiteral("refs/tags/v0.1.100"));
        const int scroll = s.view->verticalScrollBar()->value();
        QVERIFY(scroll > 0);

        // The same rows again: not a pixel moved.
        s.tree.setTags(tags(numbered("v0.1.", 200)));
        QCOMPARE(s.view->verticalScrollBar()->value(), scroll);
        QCOMPARE(keyOf(s.view->indexAt(QPoint(0, 0))), QStringLiteral("refs/tags/v0.1.100"));

        // Ten more sorted above it.
        std::vector<std::string> more = numbered("a", 10);
        const std::vector<std::string> rest = numbered("v0.1.", 200);
        more.insert(more.end(), rest.begin(), rest.end());
        s.tree.setTags(tags(more));
        QCOMPARE(keyOf(s.view->indexAt(QPoint(0, 0))), QStringLiteral("refs/tags/v0.1.100"));

        // A branch reload, everything in the same place.
        s.tree.setBranches({local("main", true), local("new")});
        QCOMPARE(keyOf(s.view->indexAt(QPoint(0, 0))), QStringLiteral("refs/tags/v0.1.100"));
    }

    // Scrolled by pixels, the top row keeps the part of it out of sight.
    void theTopRowKeepsItsPixelOffset() {
        Sidebar s;
        QVERIFY(s.view && QTest::qWaitForWindowExposed(&s.tree));
        s.view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        s.tree.setBranches({local("main", true)});
        s.tree.setTags(tags(numbered("v0.1.", 200)));
        s.view->expand(category(s.view, BranchModel::RootCategory::Tags));
        const QModelIndex row = rowOf(s.view, QStringLiteral("refs/tags/v0.1.100"));
        s.view->scrollTo(row, QAbstractItemView::PositionAtTop);
        QScrollBar* bar = s.view->verticalScrollBar();
        bar->setValue(bar->value() + 5);
        const int offset = s.view->visualRect(rowOf(s.view, QStringLiteral("refs/tags/v0.1.100"))).top();
        QCOMPARE(offset, -5);

        std::vector<std::string> more = numbered("a", 10);
        const std::vector<std::string> rest = numbered("v0.1.", 200);
        more.insert(more.end(), rest.begin(), rest.end());
        s.tree.setTags(tags(more));
        QCOMPARE(keyOf(s.view->indexAt(QPoint(0, 0))), QStringLiteral("refs/tags/v0.1.100"));
        QCOMPARE(s.view->visualRect(rowOf(s.view, QStringLiteral("refs/tags/v0.1.100"))).top(), offset);
    }

    // While filtering every category is open: one the reload brings in
    // (its first match) opens too, and one the user closed stays closed.
    void aCategoryTheFilterLetsInOpens() {
        Sidebar s;
        QVERIFY(s.view && QTest::qWaitForWindowExposed(&s.tree));
        s.tree.setBranches({local("main", true), remote("origin/v0.2-fix")});
        s.tree.setTags(tags({"other"}));
        auto* filter = s.tree.findChild<QLineEdit*>(QStringLiteral("branchTree.filter"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("v0.2"));
        QVERIFY(!category(s.view, BranchModel::RootCategory::Tags).isValid());
        QVERIFY(expanded(s.view, BranchModel::RootCategory::RemoteBranches));
        s.view->collapse(category(s.view, BranchModel::RootCategory::RemoteBranches));

        s.tree.setTags(tags({"other", "v0.2.0"}));
        QVERIFY(expanded(s.view, BranchModel::RootCategory::Tags));
        QVERIFY(rowOf(s.view, QStringLiteral("refs/tags/v0.2.0")).isValid());
        QVERIFY(!expanded(s.view, BranchModel::RootCategory::RemoteBranches));
    }

    // Making a row current scrolls to it and opens its category: the
    // restore doesn't, so a category the user closed over the selected
    // row stays closed.
    void aSelectionInAClosedCategoryLeavesItClosed() {
        Sidebar s;
        QVERIFY(s.view && QTest::qWaitForWindowExposed(&s.tree));
        s.tree.setBranches({local("main", true)});
        s.tree.setTags(tags({"v0.1.0", "v0.1.1"}));
        s.view->setCurrentIndex(rowOf(s.view, QStringLiteral("refs/tags/v0.1.0")));
        QVERIFY(expanded(s.view, BranchModel::RootCategory::Tags));
        s.view->collapse(category(s.view, BranchModel::RootCategory::Tags));

        s.tree.setTags(tags({"v0.1.0", "v0.1.1", "v0.1.2"}));
        QVERIFY(!expanded(s.view, BranchModel::RootCategory::Tags));
        QCOMPARE(keyOf(s.view->currentIndex()), QStringLiteral("refs/tags/v0.1.0"));
    }

    // View › Show tags off: the view forgot hidden rows at every reset,
    // and the filter's taking a category out and back did the same.
    void aHiddenCategoryStaysHidden() {
        Sidebar s;
        QVERIFY(s.view && QTest::qWaitForWindowExposed(&s.tree));
        s.tree.setBranches({local("main", true)});
        s.tree.setTags(tags({"v0.1.0"}));
        s.tree.setCategoryVisible(static_cast<int>(BranchModel::RootCategory::Tags), false);
        const auto tagsHidden = [&] {
            const QModelIndex tagsRow = category(s.view, BranchModel::RootCategory::Tags);
            return tagsRow.isValid() && s.view->isRowHidden(tagsRow.row(), QModelIndex());
        };
        QVERIFY(tagsHidden());

        s.tree.setTags(tags({"v0.1.0", "v0.1.1"}));
        QVERIFY(tagsHidden());
        s.tree.setBranches({local("main", true), local("other")});
        QVERIFY(tagsHidden());

        auto* filter = s.tree.findChild<QLineEdit*>(QStringLiteral("branchTree.filter"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("main"));   // takes Tags out
        filter->clear();
        QVERIFY(tagsHidden());

        s.tree.setCategoryVisible(static_cast<int>(BranchModel::RootCategory::Tags), true);
        QVERIFY(!tagsHidden());
        s.tree.setTags(tags({"v0.1.0"}));
        QVERIFY(!tagsHidden());
    }

    // Another repository starts from the default view: Local Branches
    // open, the rest closed, nothing selected. An open category with no
    // rows yet (an unborn branch's) stays open when its first comes.
    void anotherRepositoryStartsFromTheDefaults() {
        Sidebar s;
        QVERIFY(s.view && QTest::qWaitForWindowExposed(&s.tree));
        s.tree.setBranches({local("main", true)});
        s.tree.setTags(tags({"v0.1.0"}));
        s.view->collapse(category(s.view, BranchModel::RootCategory::LocalBranches));
        s.view->expand(category(s.view, BranchModel::RootCategory::Tags));
        s.view->setCurrentIndex(rowOf(s.view, QStringLiteral("refs/tags/v0.1.0")));

        s.tree.clear();
        QVERIFY(expanded(s.view, BranchModel::RootCategory::LocalBranches));
        QVERIFY(!expanded(s.view, BranchModel::RootCategory::Tags));
        QVERIFY(!s.view->currentIndex().isValid());

        s.tree.setBranches({});
        QVERIFY(expanded(s.view, BranchModel::RootCategory::LocalBranches));
        s.tree.setBranches({local("main", true)});
        QVERIFY(expanded(s.view, BranchModel::RootCategory::LocalBranches));
        QVERIFY(rowOf(s.view, QStringLiteral("refs/heads/main")).isValid());
        s.tree.setTags(tags({"v0.1.0"}));
        QVERIFY(!expanded(s.view, BranchModel::RootCategory::Tags));
    }

    // The keys the view state is kept by, and the rows they find.
    void everyRowHasAKeyThatFindsItAgain() {
        BranchModel model;
        model.setBranches({local("main", true), remote("origin/main")});
        model.setTags(tags({"main"}));   // a tag named as a branch
        gitbolt::git::StashEntry stash{0, {}, "WIP on main", {}};
        model.setStashes({stash});
        gitbolt::git::SubmoduleInfo sub;
        sub.name = "lib";
        model.setSubmodules({sub});

        QStringList keys;
        for (int cat = 0; cat < static_cast<int>(BranchModel::RootCategory::Count); ++cat) {
            const QModelIndex parent = model.index(cat, 0);
            keys << keyOf(parent);
            for (int row = 0; row < model.rowCount(parent); ++row)
                keys << keyOf(model.index(row, 0, parent));
        }
        QCOMPARE(keys, (QStringList{
                           QStringLiteral("category:0"), QStringLiteral("refs/heads/main"),
                           QStringLiteral("category:1"), QStringLiteral("refs/remotes/origin/main"),
                           QStringLiteral("category:2"), QStringLiteral("refs/tags/main"),
                           QStringLiteral("category:3"), QStringLiteral("submodule:lib"),
                           QStringLiteral("category:4"),
                           QStringLiteral("stash:") + QString::fromStdString(stash.id.toHex())}));
        for (const QString& key : keys)
            QCOMPARE(keyOf(model.indexForKey(key)), key);
        QVERIFY(!model.indexForKey(QStringLiteral("refs/heads/gone")).isValid());
        QVERIFY(!model.indexForKey(QString()).isValid());
    }
};

QTEST_MAIN(TestBranchTreeWidget)
#include "TestBranchTreeWidget.moc"
