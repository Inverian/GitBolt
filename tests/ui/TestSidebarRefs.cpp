// Tests for the sidebar while git outside GitBolt makes and deletes refs:
// they show without Refresh, the tree keeps its expansion and selection,
// and a work-tree event reloads nothing.

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "models/BranchModel.h"
#include "services/GitService.h"
#include "ui/MainWindow.h"

using gitbolt::models::BranchModel;
using gitbolt::services::GitService;
using gitbolt::ui::MainWindow;

namespace {

bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return r.ok() && r.value().success();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

QStringList comboItems(const QComboBox* combo) {
    QStringList items;
    for (int i = 0; i < combo->count(); ++i)
        items << combo->itemText(i);
    return items;
}

// The view's row of `key` (BranchModel::ItemKeyRole), at any depth.
QModelIndex rowOf(const QTreeView* view, const QString& key, const QModelIndex& parent = {}) {
    const QAbstractItemModel* model = view->model();
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (index.data(BranchModel::ItemKeyRole).toString() == key)
            return index;
        if (const QModelIndex below = rowOf(view, key, index); below.isValid())
            return below;
    }
    return {};
}

QModelIndex category(const QTreeView* view, BranchModel::RootCategory cat) {
    return rowOf(view, QStringLiteral("category:%1").arg(static_cast<int>(cat)));
}

// How many times the service listed branches and tags.
struct Loads {
    int branches = 0;
    int tags = 0;
    QObject context;

    explicit Loads(GitService* svc) {
        QObject::connect(svc, &GitService::branchesReady, &context, [this] { ++branches; });
        QObject::connect(svc, &GitService::tagsReady, &context, [this] { ++tags; });
    }
};

} // namespace

class TestSidebarRefs : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        const QString global = settingsDir_.filePath(QStringLiteral("gitconfig"));
        QVERIFY(writeFile(global, "[user]\n\tname = T\n\temail = t@example.com\n"
                                  "[commit]\n\tgpgSign = false\n"
                                  "[tag]\n\tgpgSign = false\n"
                                  "[init]\n\tdefaultBranch = main\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(global));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    void refsMadeAndDeletedOutsideShowWithoutRefresh() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"branch", "-M", "main"})));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* combo = window.findChild<QComboBox*>(QStringLiteral("toolbar.branchCombo"));
        auto* view = window.findChild<QTreeView*>(QStringLiteral("branchTree.view"));
        QVERIFY(combo && view);
        QTRY_COMPARE_WITH_TIMEOUT(comboItems(combo), QStringList{QStringLiteral("main")}, 10000);
        const auto shown = [view](const char* key) {
            return rowOf(view, QString::fromLatin1(key)).isValid();
        };

        QVERIFY(succeeded(git.run({"branch", "other"})));
        QVERIFY(succeeded(git.run({"tag", "v0.1.0"})));
        QVERIFY(succeeded(git.run({"tag", "-a", "v0.1.1", "-m", "annotated"})));
        QVERIFY(succeeded(git.run({"update-ref", "refs/remotes/origin/main", "HEAD"})));
        QTRY_VERIFY_WITH_TIMEOUT(shown("refs/heads/other"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(shown("refs/tags/v0.1.0"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(shown("refs/tags/v0.1.1"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(shown("refs/remotes/origin/main"), 15000);
        QTRY_COMPARE(comboItems(combo), (QStringList{QStringLiteral("main"), QStringLiteral("other")}));
        QCOMPARE(combo->currentText(), QStringLiteral("main"));

        QVERIFY(succeeded(git.run({"branch", "-D", "other"})));
        QVERIFY(succeeded(git.run({"tag", "-d", "v0.1.0"})));
        QVERIFY(succeeded(git.run({"update-ref", "-d", "refs/remotes/origin/main"})));
        QTRY_VERIFY_WITH_TIMEOUT(!shown("refs/heads/other"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!shown("refs/tags/v0.1.0"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!shown("refs/remotes/origin/main"), 15000);
        QTRY_COMPARE(comboItems(combo), QStringList{QStringLiteral("main")});
        QCOMPARE(combo->currentText(), QStringLiteral("main"));

        // Packed (as a clone's are), and deleted from packed-refs.
        QVERIFY(succeeded(git.run({"pack-refs", "--all"})));
        QVERIFY(!QFileInfo::exists(repo.path() + QStringLiteral("/.git/refs/tags/v0.1.1")));
        QVERIFY(succeeded(git.run({"tag", "-d", "v0.1.1"})));
        QTRY_VERIFY_WITH_TIMEOUT(!shown("refs/tags/v0.1.1"), 15000);
        QVERIFY(shown("refs/heads/main"));
    }

    // Tags and Remote Branches expanded and a tag selected, then a
    // branch made outside: the reload shows it and changes nothing else.
    void theSidebarStaysAsItWasAcrossAReload() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"branch", "-M", "main"})));
        QVERIFY(succeeded(git.run({"tag", "v0.1.0"})));
        QVERIFY(succeeded(git.run({"tag", "v0.1.1"})));
        QVERIFY(succeeded(git.run({"update-ref", "refs/remotes/origin/main", "HEAD"})));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* view = window.findChild<QTreeView*>(QStringLiteral("branchTree.view"));
        auto* model = window.findChild<BranchModel*>();
        QVERIFY(view && model);
        QTRY_VERIFY_WITH_TIMEOUT(rowOf(view, QStringLiteral("refs/tags/v0.1.1")).isValid(), 10000);
        QTRY_VERIFY(rowOf(view, QStringLiteral("refs/remotes/origin/main")).isValid());

        view->expand(category(view, BranchModel::RootCategory::RemoteBranches));
        view->expand(category(view, BranchModel::RootCategory::Tags));
        view->setCurrentIndex(rowOf(view, QStringLiteral("refs/tags/v0.1.1")));
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);

        QVERIFY(succeeded(git.run({"branch", "other"})));
        QTRY_VERIFY_WITH_TIMEOUT(rowOf(view, QStringLiteral("refs/heads/other")).isValid(), 15000);
        QVERIFY(resets.count() >= 1);
        QVERIFY(view->isExpanded(category(view, BranchModel::RootCategory::LocalBranches)));
        QVERIFY(view->isExpanded(category(view, BranchModel::RootCategory::RemoteBranches)));
        QVERIFY(view->isExpanded(category(view, BranchModel::RootCategory::Tags)));
        QCOMPARE(view->currentIndex().data(BranchModel::ItemKeyRole).toString(),
                 QStringLiteral("refs/tags/v0.1.1"));
        QCOMPARE(view->selectionModel()->selectedIndexes().size(), 1);

        // A reload for a remote branch leaves the toolbar's list of local
        // branches alone: not emptied and filled again under an open list.
        auto* combo = window.findChild<QComboBox*>(QStringLiteral("toolbar.branchCombo"));
        QVERIFY(combo);
        QTRY_COMPARE(comboItems(combo), (QStringList{QStringLiteral("main"), QStringLiteral("other")}));
        QSignalSpy comboResets(combo->model(), &QAbstractItemModel::modelReset);
        QSignalSpy comboRemoved(combo->model(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy comboInserted(combo->model(), &QAbstractItemModel::rowsInserted);
        QVERIFY(succeeded(git.run({"update-ref", "refs/remotes/origin/feature", "HEAD"})));
        QTRY_VERIFY_WITH_TIMEOUT(rowOf(view, QStringLiteral("refs/remotes/origin/feature")).isValid(),
                                 15000);
        QCOMPARE(comboResets.count() + comboRemoved.count() + comboInserted.count(), 0);
        QCOMPARE(combo->currentText(), QStringLiteral("main"));
    }

    // A file written in the work tree and then staged: status refreshes,
    // neither branches nor tags are listed, the tree isn't reset.
    void aStatusChangeReloadsNoRefs() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"tag", "v0.1.0"})));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* model = window.findChild<BranchModel*>();
        QVERIFY(model);
        Loads loads(svc);
        QTRY_VERIFY_WITH_TIMEOUT(
            model->indexForKey(QStringLiteral("refs/tags/v0.1.0")).isValid(), 10000);
        QTest::qWait(1500);   // what the open itself set off

        loads.branches = loads.tags = 0;
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        int statuses = 0;
        QObject context;
        connect(svc, &GitService::statusReady, &context, [&statuses] { ++statuses; });

        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        QTRY_VERIFY_WITH_TIMEOUT(statuses > 0, 15000);
        statuses = 0;
        QVERIFY(succeeded(git.run({"add", "b.txt"})));
        QTRY_VERIFY_WITH_TIMEOUT(statuses > 0, 15000);
        QTest::qWait(1000);
        QCOMPARE(loads.branches, 0);
        QCOMPARE(loads.tags, 0);
        QCOMPARE(resets.count(), 0);

        // A tag lists the tags alone.
        QVERIFY(succeeded(git.run({"tag", "v0.1.1"})));
        QTRY_VERIFY_WITH_TIMEOUT(loads.tags > 0, 15000);
        QTest::qWait(1000);
        QCOMPARE(loads.branches, 0);
    }

private:
    static GitService* openIn(MainWindow& window, const QString& path) {
        window.show();
        if (!QTest::qWaitForWindowExposed(&window))
            return nullptr;
        window.openRepositoryAtPath(path);
        auto* svc = window.findChild<GitService*>();
        auto* commit = window.findChild<QAction*>(QStringLiteral("act.commit"));
        if (!svc || !commit)
            return nullptr;
        const bool open = QTest::qWaitFor([&] {
            return svc->isOpen() && !svc->openPending() && commit->isEnabled();
        }, 15000);
        return open ? svc : nullptr;
    }

    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestSidebarRefs)
#include "TestSidebarRefs.moc"
