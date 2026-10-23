//
// TestFileTree — the inspector's file tree survives a rebuild under its
// own context menu.
//
// menu.exec() runs a nested event loop. A hand-off of another
// repository, or a log refresh that moves the selected commit, handled
// in it clears the tree and builds the next one. Expand All and
// Collapse All held the right-clicked directory as a plain proxy index,
// which then pointed into mappings the proxy had freed. Reading them
// usually crashed, and always does under Guard Malloc, which this test
// runs with on macOS (MALLOC_PERTURB_ on glibc, to the same end); where
// it didn't, the action could expand or collapse a directory of the new
// tree that the user never picked.
//
// Its header counts the commit's files and folders in English:
// "1 file, 1 folder", not "1 files, 1 directories".
//
// The tree is walked on the service's read thread (GitService::
// readAsync), so the tests wait for it to come in.
//

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QTest>
#include <QTimer>
#include <QTreeView>

#include "../TestRepoHelper.h"
#include "services/GitService.h"
#include "util/Translations.h"
#include "widgets/FileTreeWidget.h"

using gitbolt::services::GitService;
using gitbolt::test::TestRepo;
using gitbolt::widgets::FileTreeWidget;

namespace {

// The open context menu, found among the top-level widgets.
QMenu* openMenu() {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        return menu;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* menu = qobject_cast<QMenu*>(w); menu && menu->isVisible())
            return menu;
    }
    return nullptr;
}

// Every index under `parent` that the view shows expanded.
QStringList expanded(const QTreeView* view, const QModelIndex& parent = {}) {
    QStringList names;
    const QAbstractItemModel* model = view->model();
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (view->isExpanded(index))
            names << index.data().toString();
        names << expanded(view, index);
    }
    return names;
}

} // namespace

class TestFileTree : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Two commits whose trees both start with a directory.
        repo_.writeFile(QStringLiteral("dir/sub/file.txt"), "a\n");
        repo_.writeFile(QStringLiteral("dir/other.txt"), "b\n");
        QVERIFY(repo_.stageAll().ok());
        auto first = repo_.commit(QStringLiteral("first"));
        QVERIFY(first.ok());
        first_ = *first;
        repo_.writeFile(QStringLiteral("more/deeper/file.txt"), "c\n");
        QVERIFY(repo_.stageAll().ok());
        auto second = repo_.commit(QStringLiteral("second"));
        QVERIFY(second.ok());
        second_ = *second;
        // The app's English plural forms, as main.cpp installs them.
        QCOMPARE(gitbolt::util::installEnglishPlurals(), gitbolt::util::hasEnglishPlurals());
    }

    void headerCountsFilesAndDirectories() {
        if (!gitbolt::util::hasEnglishPlurals())
            QSKIP("built without Qt LinguistTools (or with Qt before 6.7): no English plural forms");
        TestRepo single;
        single.writeFile(QStringLiteral("dir/file.txt"), "a\n");
        QVERIFY(single.stageAll().ok());
        auto only = single.commit(QStringLiteral("only"));
        QVERIFY(only.ok());

        GitService svc;
        QVERIFY(svc.openRepository(single.path()));
        FileTreeWidget tree;
        tree.setGitService(&svc);
        auto* header = tree.findChild<QLabel*>(QStringLiteral("fileTree.previewHeader"));
        QVERIFY(header);
        // The walk comes from the service's read thread.
        tree.setCommit(*only);
        QTRY_VERIFY2(header->text().startsWith(QStringLiteral("1 file, 1 folder — ")),
                     qPrintable(header->text()));

        // dir/, dir/sub/, dir/sub/file.txt and dir/other.txt
        GitService other;
        QVERIFY(other.openRepository(repo_.path()));
        tree.setGitService(&other);
        tree.setCommit(first_);
        QTRY_VERIFY2(header->text().startsWith(QStringLiteral("2 files, 2 folders — ")),
                     qPrintable(header->text()));
    }

    void menuActionsSurviveARebuild_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("Expand All") << QStringLiteral("Expand All");
        QTest::newRow("Collapse All") << QStringLiteral("Collapse All");
    }

    void menuActionsSurviveARebuild() {
        QFETCH(QString, action);

        GitService svc;
        QVERIFY(svc.openRepository(repo_.path()));
        FileTreeWidget tree;
        tree.setGitService(&svc);
        tree.setCommit(first_);
        tree.resize(640, 400);
        tree.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tree));

        auto* view = tree.findChild<QTreeView*>(QStringLiteral("fileTree.view"));
        QVERIFY(view);
        QTRY_VERIFY(view->model()->rowCount() > 0);   // the walk is in
        const QModelIndex dir = view->model()->index(0, 0);
        QCOMPARE(dir.data().toString(), QStringLiteral("dir"));
        QCOMPARE(expanded(view), QStringList{QStringLiteral("dir")});   // the top level

        // While the menu is open: the next commit's tree replaces this
        // one, then the user picks the action.
        bool picked = false;
        QTimer::singleShot(0, this, [&] {
            QMenu* menu = openMenu();
            if (!menu)
                return;
            tree.clear();
            tree.setCommit(second_);
            if (!QTest::qWaitFor([view] { return view->model()->rowCount() > 0; })) {
                menu->close();   // no new tree: picked stays false
                return;
            }
            for (QAction* a : menu->actions()) {
                if (a->text() == action) {
                    a->trigger();
                    picked = true;
                }
            }
            menu->close();
        });
        emit view->customContextMenuRequested(view->visualRect(dir).center());
        QVERIFY(picked);

        // The directory the menu was for is gone with its tree, so the
        // new one is left as it was built: its top level expanded, and
        // nothing below it.
        QCOMPARE(expanded(view), (QStringList{QStringLiteral("dir"), QStringLiteral("more")}));
    }

private:
    TestRepo repo_;
    gitbolt::git::ObjectId first_;
    gitbolt::git::ObjectId second_;
};

QTEST_MAIN(TestFileTree)
#include "TestFileTree.moc"
