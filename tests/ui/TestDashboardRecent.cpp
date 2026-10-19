//
// TestDashboardRecent — the dashboard's recent list fills each row with
// its own repository's probe, however often the list is rebuilt.
//
// Each row's Branch, Status, ↑↓ and Committed come from a probe on the
// thread pool. Its handler used to find its row by the row's pointer OR
// its path, and a rebuild while probes were in flight (any
// settingsChanged: an open, Forget, a Settings Apply) left that pointer
// dangling. The allocator hands freed rows' addresses to new rows, so a
// stale probe could match a new row of another repository by address,
// ahead of its own row, and paint its branch there.
//
// Whether an address comes back to a row that is ahead of its own is
// up to the allocator: one rebuild of four rows crossed a probe in about
// one run out of four on macOS. So the list is full (ten repositories,
// the default cap) and built ten times before any probe is handled,
// which crossed one in 99 runs out of 100, and that is done three times
// over. A serial pool lands each build's probes in order, as a busy pool
// would. Each repository is on a branch of its own, so any Branch value
// written into another repository's row shows.
//
// And a repository whose status can't be read (a tree nested too deep
// for libgit2) has a Status of "—", with why on hover, not a blank one,
// which reads as clean.
//

#include <QAbstractItemModel>
#include <QApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QHash>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTreeWidget>

#include <memory>
#include <string>
#include <vector>

#include "../TestRepoHelper.h"
#include "conf/SettingsService.h"
#include "ui/DashboardView.h"

using gitbolt::conf::SettingsService;
using gitbolt::test::TestRepo;

class TestDashboardRecent : public QObject {
    Q_OBJECT

private slots:
    // GitBolt.ini in a folder of the test's own.
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    void rebuildKeepsProbesOnTheirOwnRows() {
        constexpr int kRepos = 10;
        constexpr int kBuilds = 10;
        constexpr int kRounds = 3;

        // repo 0 to repo 9 on branches b0 to b9, listed in that order.
        std::vector<std::unique_ptr<TestRepo>> repos;
        QHash<QString, QString> branchOf;
        QStringList paths;
        for (int i = 0; i < kRepos; ++i) {
            auto repo = std::make_unique<TestRepo>();
            auto head = repo->writeAndCommit(QStringLiteral("a.txt"), "a\n",
                                             QStringLiteral("base"));
            QVERIFY(head.ok());
            const std::string branch = "b" + std::to_string(i);
            QVERIFY(repo->repo().createBranch(branch, *head).ok());
            QVERIFY(repo->repo().checkout(branch).ok());
            const QString path = QDir::toNativeSeparators(repo->path());
            branchOf.insert(path, QString::fromStdString(branch));
            paths << path;
            repos.push_back(std::move(repo));
        }
        SettingsService settings;
        for (auto it = paths.crbegin(); it != paths.crend(); ++it)
            settings.addRecentRepository(*it);
        QCOMPARE(settings.recentRepositories(), paths);

        QThreadPool* pool = QThreadPool::globalInstance();
        const int threads = pool->maxThreadCount();
        const auto restorePool = qScopeGuard([pool, threads] {
            pool->waitForDone();
            pool->setMaxThreadCount(threads);
        });
        pool->setMaxThreadCount(1);

        QStringList crossed;
        for (int round = 0; round < kRounds && crossed.isEmpty(); ++round) {
            // The first build, in the constructor, then the rest before
            // any probe lands.
            gitbolt::ui::DashboardView view(&settings);
            auto* list = view.findChild<QTreeWidget*>();
            QVERIFY(list);
            connect(list->model(), &QAbstractItemModel::dataChanged, &view,
                    [&crossed, &branchOf, list](const QModelIndex& topLeft,
                                                const QModelIndex& bottomRight) {
                if (topLeft.column() > 1 || bottomRight.column() < 1)
                    return;
                for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
                    const QModelIndex cell = list->model()->index(row, 1, topLeft.parent());
                    const QString path = cell.siblingAtColumn(0).data(Qt::UserRole).toString();
                    const QString branch = cell.data().toString();
                    if (path.isEmpty() || branch == QStringLiteral("…"))
                        continue;   // a row being built
                    if (branch != branchOf.value(path))
                        crossed << QStringLiteral("%1 painted into the row of %2")
                                       .arg(branch, path);
                }
            });
            for (int build = 1; build < kBuilds; ++build)
                view.refreshRecentList();

            // Every probe landed: each deletes its watcher once handled.
            QTRY_VERIFY_WITH_TIMEOUT(view.findChildren<QFutureWatcherBase*>().isEmpty(), 30000);
            QCOMPARE(list->topLevelItemCount(), kRepos);
            for (int i = 0; i < kRepos; ++i) {
                const QTreeWidgetItem* item = list->topLevelItem(i);
                QCOMPARE(item->text(1), branchOf.value(item->data(0, Qt::UserRole).toString()));
            }
        }
        QVERIFY2(crossed.isEmpty(), qPrintable(crossed.join(QLatin1Char('\n'))));
    }

    // A repository whose status libgit2 turns down (HEAD's tree nested
    // deeper than the bundled one goes): its Status is "—", not known,
    // and says why on hover. It was blank, which reads as clean.
    void aStatusThatCantBeReadIsNotShownClean() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        repo.commitDeepTree(2350);
        const QString path = QDir::toNativeSeparators(repo.path());
        SettingsService settings;
        settings.addRecentRepository(path);

        gitbolt::ui::DashboardView view(&settings);
        auto* list = view.findChild<QTreeWidget*>();
        QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(view.findChildren<QFutureWatcherBase*>().isEmpty(), 30000);
        const QTreeWidgetItem* item = nullptr;
        for (int i = 0; i < list->topLevelItemCount() && !item; ++i) {
            if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == path)
                item = list->topLevelItem(i);
        }
        QVERIFY(item);
        QCOMPARE(item->text(2), QStringLiteral("—"));
        QCOMPARE(item->toolTip(2),
                 QStringLiteral("Status couldn't be read: tree too deep: more than 2048 levels"));
#endif
    }

private:
    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestDashboardRecent)
#include "TestDashboardRecent.moc"
