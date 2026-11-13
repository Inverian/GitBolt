//
// TestSmallScreen — GitBolt on a small screen: the window opens whole on
// it, and the welcome screen's cards fit a narrow window.
//
// Windows 11 at 1024x768 and 125% leaves 819x566 for windows, with fonts
// at 96 dpi (tests/fixtures/small-screen.json). The welcome screen's
// three fixed 290 px cards held the window over 1000 px wide, and neither
// the startup size (1024x700 at least) nor a saved geometry was fitted
// to the screen: the third card ran off its right edge.
//

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "../TestRepoHelper.h"
#include "conf/SettingsService.h"
#include "dialogs/SettingsDialog.h"
#include "services/GitService.h"
#include "ui/DashboardView.h"
#include "ui/MainWindow.h"
#include "widgets/WindowPlacement.h"

using gitbolt::conf::SettingsService;
using gitbolt::ui::DashboardView;
using gitbolt::ui::MainWindow;

namespace {

const char* const kCards[] = {"dashboard.openCard", "dashboard.cloneCard",
                              "dashboard.initCard"};

QRect rectIn(const QWidget* widget, const QWidget* ancestor) {
    return {widget->mapTo(ancestor, QPoint(0, 0)), widget->size()};
}

QString str(const QRect& r) {
    return QStringLiteral("%1x%2+%3+%4").arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y());
}

} // namespace

class TestSmallScreen : public QObject {
    Q_OBJECT

    QTemporaryDir settingsDir_;
    QTemporaryDir homeDir_;

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QVERIFY(homeDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        // Settings' Apply runs `git config --global`: a file of our own.
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(homeDir_.filePath(QStringLiteral("gitconfig"))));
        qputenv("HOME", QFile::encodeName(homeDir_.path()));
        // The fixture's screen, or the window tests prove nothing.
        const QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        QCOMPARE(screen->availableGeometry(), QRect(0, 0, 819, 566));
    }

    // Every test starts from an empty GitBolt.ini.
    void init() {
        SettingsService settings;
        QSettings(settings.fileName(), QSettings::IniFormat).clear();
    }

    void fitWindowFrame_data() {
        QTest::addColumn<QRect>("frame");
        QTest::addColumn<QRect>("available");
        QTest::addColumn<QRect>("fitted");

        const QRect small(0, 0, 819, 566);
        QTest::newRow("fits") << QRect(20, 30, 700, 500) << small << QRect(20, 30, 700, 500);
        QTest::newRow("exactly") << small << small << small;
        QTest::newRow("too big") << QRect(26, 26, 1400, 900) << small << small;
        QTest::newRow("too wide") << QRect(100, 20, 1024, 500) << small
                                  << QRect(0, 20, 819, 500);
        QTest::newRow("past the right") << QRect(500, 20, 600, 400) << small
                                        << QRect(219, 20, 600, 400);
        QTest::newRow("past the bottom") << QRect(10, 400, 600, 400) << small
                                         << QRect(10, 166, 600, 400);
        QTest::newRow("above and left") << QRect(-50, -20, 600, 400) << small
                                        << QRect(0, 0, 600, 400);
        // A screen left of the primary, under a macOS menu bar.
        const QRect left(-1280, 25, 1280, 775);
        QTest::newRow("other screen") << QRect(-1300, 0, 1400, 900) << left << left;
        QTest::newRow("other screen, fits") << QRect(-1200, 100, 800, 600) << left
                                            << QRect(-1200, 100, 800, 600);
        QTest::newRow("no screen") << QRect(10, 10, 2000, 2000) << QRect()
                                   << QRect(10, 10, 2000, 2000);
    }

    void fitWindowFrame() {
        QFETCH(QRect, frame);
        QFETCH(QRect, available);
        QFETCH(QRect, fitted);
        QCOMPARE(gitbolt::widgets::fitWindowFrame(frame, available), fitted);
    }

    // Each card whole, inside what the dashboard shows, beside the
    // others, its labels whole inside it; at 1400 px today's 290 px
    // cards, 220 px tall unless a font needs more.
    void cardsFitTheDashboard_data() {
        QTest::addColumn<int>("width");
        QTest::newRow("800") << 800;
        QTest::newRow("1000") << 1000;
        QTest::newRow("1400") << 1400;
    }

    void cardsFitTheDashboard() {
        QFETCH(int, width);
        SettingsService settings;
        DashboardView view(&settings);
        view.resize(width, 600);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTRY_COMPARE(view.width(), width);
        auto* scroll = view.findChild<QScrollArea*>();
        QVERIFY(scroll);
        QVERIFY2(!scroll->horizontalScrollBar()->isVisible(), "the page scrolls sideways");
        QVERIFY(cardsShown(&view, scroll->viewport()));
        if (width >= 1400) {
            for (const char* name : kCards) {
                const auto* card = view.findChild<QPushButton*>(QString::fromLatin1(name));
                QCOMPARE(card->width(), 290);
                QVERIFY(card->height() >= 220);
            }
        }
    }

    // The dashboard asks the window for no more room than a scroll area
    // does: it scrolls instead. Its page, at most 760 px wide, fits an
    // 800 px window with a vertical scroll bar and the window's frame.
    void dashboardNeedsLittleRoom() {
        SettingsService settings;
        DashboardView view(&settings);
        const QSize least = view.minimumSizeHint();
        QVERIFY2(least.width() <= 200 && least.height() <= 200,
                 qPrintable(QStringLiteral("%1x%2").arg(least.width()).arg(least.height())));
        auto* page = view.findChild<QWidget*>(QStringLiteral("dashboard.page"));
        QVERIFY(page);
        QVERIFY2(page->minimumSizeHint().width() <= 760,
                 qPrintable(QString::number(page->minimumSizeHint().width())));
    }

    // The first launch's size (the 1024x700 floor on this screen), a
    // larger startup size, and a saved geometry larger than the screen:
    // the window opens inside the screen, every card on it.
    void windowOpensOnTheScreen_data() {
        QTest::addColumn<bool>("restoreLast");
        QTest::addColumn<QSize>("startup");
        QTest::addColumn<QRect>("saved");
        QTest::newRow("first launch") << true << QSize() << QRect();
        QTest::newRow("startup size") << false << QSize(1600, 1000) << QRect();
        QTest::newRow("saved geometry") << true << QSize() << QRect(300, 200, 1400, 900);
    }

    void windowOpensOnTheScreen() {
        QFETCH(bool, restoreLast);
        QFETCH(QSize, startup);
        QFETCH(QRect, saved);
        {
            SettingsService settings;
            settings.setRestoreLastWindowSize(restoreLast);
            if (startup.isValid()) {
                settings.setStartupWindowWidth(startup.width());
                settings.setStartupWindowHeight(startup.height());
            }
            if (saved.isValid()) {
                QWidget before;
                before.setGeometry(saved);
                settings.saveWindowGeometry(before.saveGeometry());
            }
        }
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QRect avail = window.screen()->availableGeometry();
        QVERIFY2(avail.contains(window.frameGeometry()),
                 qPrintable(QStringLiteral("frame %1 on %2")
                                .arg(str(window.frameGeometry()), str(avail))));

        auto* view = window.findChild<DashboardView*>();
        QVERIFY(view && view->isVisible());
        QVERIFY(cardsShown(view, view->findChild<QScrollArea*>()->viewport()));
        for (const char* name : kCards) {
            const auto* card = view->findChild<QPushButton*>(QString::fromLatin1(name));
            const QRect onScreen(card->mapToGlobal(QPoint(0, 0)), card->size());
            QVERIFY2(avail.contains(onScreen), name);
        }
    }

    // With a repository open the window still fits: the repository view,
    // the toolbar's branch and filter boxes and the status bar's path (a
    // long one, wider than the screen) ask for less than the screen, so
    // the window isn't pushed past its edge.
    void repositoryWindowFits() {
        // In TestRepo's folder, which keeps libgit2 off the user's config.
        gitbolt::test::TestRepo outer;
        const QString path = outer.path() + QStringLiteral("/a-folder-name-that-goes-on").repeated(5);
        QVERIFY(QDir().mkpath(path));
        QVERIFY(gitbolt::git::Repository::init(path.toStdString()).ok());
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openRepositoryAtPath(path);
        auto* svc = window.findChild<gitbolt::services::GitService*>();
        auto* commit = window.findChild<QAction*>(QStringLiteral("act.commit"));
        QVERIFY(svc && commit);
        QTRY_VERIFY_WITH_TIMEOUT(svc->isOpen() && !svc->openPending() && commit->isEnabled(),
                                 15000);
        QTRY_VERIFY(!window.findChild<DashboardView*>()->isVisible());

        const QRect avail = window.screen()->availableGeometry();
        const QSize least = window.minimumSizeHint().expandedTo(window.minimumSize());
        QVERIFY2(least.width() <= avail.width() && least.height() <= avail.height(),
                 qPrintable(QStringLiteral("%1x%2").arg(least.width()).arg(least.height())));
        QVERIFY2(avail.contains(window.frameGeometry()), qPrintable(str(window.frameGeometry())));
    }

    // Settings' Apply, with "Restore previous window size" off, resizes
    // the window to the default size there (1024x700 at least): it stays
    // on the screen too.
    void settingsApplyKeepsTheWindowOnScreen() {
        SettingsService settings;
        settings.setRestoreLastWindowSize(false);
        settings.setStartupWindowWidth(1600);
        settings.setStartupWindowHeight(1000);
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.resize(600, 480);
        QTRY_COMPARE(window.width(), 600);

        gitbolt::dialogs::SettingsDialog dialog(&settings, nullptr, &window);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "apply"));
        const QRect avail = window.screen()->availableGeometry();
        QVERIFY2(window.width() > 600, "Apply didn't resize the window");
        QVERIFY2(avail.contains(window.frameGeometry()), qPrintable(str(window.frameGeometry())));
    }

private:
    // The cards whole inside `shown`, apart, and their labels whole.
    static bool cardsShown(const QWidget* view, const QWidget* shown) {
        const QRect area = rectIn(shown, view);
        QList<QRect> placed;
        for (const char* name : kCards) {
            const auto* card = view->findChild<QPushButton*>(QString::fromLatin1(name));
            if (!card) {
                qWarning("no %s", name);
                return false;
            }
            const QRect r = rectIn(card, view);
            if (!area.contains(r)) {
                qWarning("%s %s is outside %s", name, qPrintable(str(r)), qPrintable(str(area)));
                return false;
            }
            for (const QRect& other : std::as_const(placed)) {
                if (other.intersects(r)) {
                    qWarning("%s %s overlaps %s", name, qPrintable(str(r)), qPrintable(str(other)));
                    return false;
                }
            }
            placed << r;
            for (const QLabel* label : card->findChildren<QLabel*>()) {
                // A wrapped label's minimum width is its longest word's.
                const int needs = label->wordWrap() ? label->heightForWidth(label->width())
                                                    : label->sizeHint().height();
                if (!card->rect().contains(rectIn(label, card))
                    || label->minimumSizeHint().width() > label->width()
                    || needs > label->height()) {
                    qWarning("%s's \"%s\" is cut: %s, needs %dx%d", name,
                             qPrintable(label->text()), qPrintable(str(rectIn(label, card))),
                             label->minimumSizeHint().width(), needs);
                    return false;
                }
            }
        }
        return true;
    }
};

QTEST_MAIN(TestSmallScreen)
#include "TestSmallScreen.moc"
