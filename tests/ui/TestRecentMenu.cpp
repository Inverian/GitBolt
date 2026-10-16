//
// TestRecentMenu — File > Recent Repositories follows the recent list
// wherever it changes, through a real MainWindow's own SettingsService.
//
// The submenu used to be rebuilt only at startup and when a repository
// opened. The dashboard's Forget and Clear Recent change the list too,
// and the submenu kept the entries they removed, still enabled and
// still opening the forgotten path:
//
//   - Forget one entry: it leaves the submenu, the rest renumbered;
//   - an open adds one: it leads the submenu;
//   - Clear Recent: only the disabled placeholder is left.
//

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "conf/SettingsService.h"
#include "git/Repository.h"
#include "ui/MainWindow.h"

using gitbolt::conf::SettingsService;

namespace {

// The Recent entries' paths, in menu order. Only entries named
// file.recent.<n> in sequence count: the name is how the test bridge
// finds them.
QStringList entries(const QMenu* menu) {
    QStringList paths;
    for (QAction* a : menu->actions()) {
        if (a->objectName() == QStringLiteral("file.recent.%1").arg(paths.size()))
            paths << a->text();
    }
    return paths;
}

} // namespace

class TestRecentMenu : public QObject {
    Q_OBJECT

private slots:
    // GitBolt.ini in a folder of the test's own.
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QVERIFY(reposDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    void followsTheRecentList() {
        const QString first = makeRepo(QStringLiteral("first"));
        const QString second = makeRepo(QStringLiteral("second"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        {
            // Listed before the window starts, the latest first.
            SettingsService seed;
            seed.addRecentRepository(second);
            seed.addRecentRepository(first);
        }

        gitbolt::ui::MainWindow window;
        auto* settings = window.findChild<SettingsService*>();
        QVERIFY(settings);
        auto* firstEntry = window.findChild<QAction*>(QStringLiteral("file.recent.0"));
        QVERIFY(firstEntry);
        auto* menu = qobject_cast<QMenu*>(firstEntry->parent());
        QVERIFY(menu);
        QCOMPARE(entries(menu), (QStringList{first, second}));

        // Forget, as the dashboard's context menu does it.
        settings->removeRecentRepository(second);
        QTRY_COMPARE(entries(menu), QStringList{first});
        QVERIFY(!window.findChild<QAction*>(QStringLiteral("file.recent.1")));

        // An open lists its repository first.
        const QString third = makeRepo(QStringLiteral("third"));
        QVERIFY(!third.isEmpty());
        settings->addRecentRepository(third);
        QTRY_COMPARE(entries(menu), (QStringList{third, first}));

        // Clear Recent: the disabled placeholder alone.
        settings->clearRecentRepositories();
        QTRY_VERIFY(entries(menu).isEmpty());
        const QList<QAction*> left = menu->actions();
        QCOMPARE(left.size(), 1);
        QVERIFY(!left.first()->isEnabled());
    }

private:
    // A repository, as the window drops Recent entries that aren't
    // one. Returns the path as SettingsService lists it.
    QString makeRepo(const QString& name) {
        const QString path = reposDir_.filePath(name);
        if (!QDir().mkpath(path) || !gitbolt::git::Repository::init(path.toStdString()).ok())
            return {};
        return QDir::toNativeSeparators(path);
    }

    QTemporaryDir settingsDir_;
    QTemporaryDir reposDir_;
};

QTEST_MAIN(TestRecentMenu)
#include "TestRecentMenu.moc"
