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
// test_small_screen_large_text runs dashboardIsWhole again with fonts at
// 144 dpi, unscaled (fixtures/small-screen-large-text.json), where the
// Contribute row is the page's widest.
//
// On Windows the offscreen platform finds fonts only through
// QT_QPA_FONTDIR (tests/CMakeLists.txt points it at Windows' own).
//

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

#include <memory>

#include "../TestRepoHelper.h"
#include "conf/SettingsService.h"
#include "conf/WindowPlacement.h"
#include "dialogs/CloneDialog.h"
#include "dialogs/CommitDialog.h"
#include "dialogs/SettingsDialog.h"
#include "services/GitService.h"
#include "ui/DashboardView.h"
#include "ui/MainWindow.h"

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
        // Real fonts too: without any, each glyph is a box as wide as the
        // font is tall, and every width here is wrong.
        const QFont font = QApplication::font();
        const QFontMetrics metrics(font);
        qInfo("Font: %s, %d px; \"Clone a Repository\" is %d px wide",
              qPrintable(QFontInfo(font).family()), QFontInfo(font).pixelSize(),
              metrics.horizontalAdvance(QStringLiteral("Clone a Repository")));
        QVERIFY2(!QFontDatabase::families().isEmpty(), "no fonts: on Windows, see QT_QPA_FONTDIR");
        QVERIFY2(metrics.horizontalAdvance(QStringLiteral("iiii")) * 2
                     < metrics.horizontalAdvance(QStringLiteral("WWWW")),
                 "an i is as wide as a W: the glyphs are boxes");
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
        QCOMPARE(gitbolt::conf::fitWindowFrame(frame, available), fitted);
    }

    // A window still to be placed by the system: one that fits is left to
    // it (macOS centres it; fitting it there put it in the screen's top
    // left corner, its frame's position being (0,0) until then), one too
    // large is fitted and centred. A placed one moves just far enough.
    void fitOnScreen() {
        QWidget fits;
        fits.resize(600, 400);
        gitbolt::conf::fitOnScreen(&fits);
        QVERIFY(!fits.testAttribute(Qt::WA_Moved));
        QCOMPARE(fits.size(), QSize(600, 400));

        QWidget tooLarge;
        tooLarge.resize(1400, 500);
        gitbolt::conf::fitOnScreen(&tooLarge);
        QCOMPARE(tooLarge.geometry(), QRect(0, 33, 819, 500));

        QWidget placed;
        placed.setGeometry(500, 300, 600, 400);
        gitbolt::conf::fitOnScreen(&placed);
        QCOMPARE(placed.geometry(), QRect(219, 166, 600, 400));

        QWidget maximized;
        maximized.setGeometry(500, 300, 600, 400);
        maximized.setWindowState(Qt::WindowMaximized);
        gitbolt::conf::fitOnScreen(&maximized);
        QCOMPARE(maximized.geometry(), QRect(500, 300, 600, 400));
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

    // At every width, each card whole on the page (it scrolls sideways
    // below its least width), its labels whole, the cards' icons, titles
    // and descriptions in line across the row although titles wrap at
    // some widths, and the Contribute row's links on the page (with
    // large text the row is the page's widest).
    void dashboardIsWhole_data() {
        QTest::addColumn<int>("width");
        for (int width : {640, 700, 730, 760, 800, 900, 1000, 1400})
            QTest::newRow(qPrintable(QString::number(width))) << width;
    }

    void dashboardIsWhole() {
        QFETCH(int, width);
        SettingsService settings;
        DashboardView view(&settings);
        view.resize(width, 600);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTRY_COMPARE(view.width(), width);
        auto* page = view.findChild<QWidget*>(QStringLiteral("dashboard.page"));
        QVERIFY(page);
        QVERIFY(cardsShown(&view, page));
        QVERIFY(cardsLineUp(&view));

        for (const QAbstractButton* button :
             page->findChildren<QAbstractButton*>(Qt::FindDirectChildrenOnly)) {
            if (!button->isVisible())
                continue;
            QVERIFY2(page->rect().contains(button->geometry()),
                     qPrintable(button->text() + QLatin1Char(' ') + str(button->geometry())));
        }
    }

    // With recent repositories, a laptop's window (1280 x 720) shows the
    // whole welcome screen: the recent list gives up height, as it did
    // before the page scrolled, rather than the page scrolling the
    // Contribute row out of sight. Only shorter than the page's least
    // height does it scroll.
    void recentListShrinksBeforeThePageScrolls() {
        gitbolt::test::TestRepo outer;
        {
            SettingsService settings;
            for (int i = 1; i <= 6; ++i) {
                const QString path = outer.path() + QStringLiteral("/project-%1").arg(i);
                QVERIFY(QDir().mkpath(path));
                QVERIFY(gitbolt::git::Repository::init(path.toStdString()).ok());
                settings.addRecentRepository(path);
            }
        }
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.resize(1280, 720);
        QTRY_COMPARE(window.size(), QSize(1280, 720));
        auto* view = window.findChild<DashboardView*>();
        auto* scroll = view->findChild<QScrollArea*>();
        auto* recent = view->findChild<QTreeWidget*>();
        QVERIFY(scroll && recent);
        QTRY_COMPARE(recent->topLevelItemCount(), 6);
        QTRY_VERIFY(recent->isVisible());
        QTest::qWait(50);  // the layout requests the resize posted
        QVERIFY2(!scroll->verticalScrollBar()->isVisible(),
                 qPrintable(QStringLiteral("the page scrolls: %1 high in %2")
                                .arg(scroll->widget()->height())
                                .arg(scroll->viewport()->height())));
        QVERIFY2(recent->height() < recent->sizeHint().height(),
                 qPrintable(QString::number(recent->height())));
        const QRect shown = rectIn(scroll->viewport(), view);
        for (const QToolButton* link : scroll->widget()->findChildren<QToolButton*>()) {
            QVERIFY2(shown.contains(rectIn(link, view)),
                     qPrintable(link->text() + QLatin1Char(' ') + str(rectIn(link, view))));
        }

        window.resize(1280, 560);
        QTRY_COMPARE(window.height(), 560);
        QTRY_VERIFY(scroll->verticalScrollBar()->isVisible());
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

    // The dialogs a first launch reaches (Clone, on the welcome screen;
    // Settings; Commit, once a repository is open) open inside the screen:
    // their default size (1000 x 800), or a saved one larger than the
    // screen, is fitted as they show. QDialog only moves a dialog. Clone
    // sizes itself from its contents now, and ignores a saved size.
    void dialogsFitTheScreen_data() {
        QTest::addColumn<QString>("dialog");
        QTest::addColumn<bool>("saved");
        QTest::newRow("clone") << QStringLiteral("clone") << false;
        QTest::newRow("clone, saved larger") << QStringLiteral("clone") << true;
        QTest::newRow("settings") << QStringLiteral("settings") << false;
        QTest::newRow("commit") << QStringLiteral("commit") << false;
    }

    void dialogsFitTheScreen() {
        QFETCH(QString, dialog);
        QFETCH(bool, saved);
        SettingsService settings;
        if (saved) {
            QWidget before;
            before.setGeometry(100, 80, 1400, 1000);
            QSettings(settings.fileName(), QSettings::IniFormat)
                .setValue(QStringLiteral("layout/dialog/%1/geom").arg(dialog), before.saveGeometry());
        }
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        gitbolt::test::TestRepo repo;
        gitbolt::services::GitService svc;
        std::unique_ptr<QDialog> dlg;
        if (dialog == QLatin1String("clone")) {
            dlg = std::make_unique<gitbolt::dialogs::CloneDialog>(&window);
        } else if (dialog == QLatin1String("settings")) {
            dlg = std::make_unique<gitbolt::dialogs::SettingsDialog>(&settings, nullptr, &window);
        } else {
            QVERIFY(svc.openRepository(repo.path()));
            dlg = std::make_unique<gitbolt::dialogs::CommitDialog>(&svc, &settings, &window);
        }
        dlg->show();
        QVERIFY(QTest::qWaitForWindowExposed(dlg.get()));
        const QRect avail = window.screen()->availableGeometry();
        QVERIFY2(avail.contains(dlg->frameGeometry()),
                 qPrintable(QStringLiteral("frame %1 (least %2x%3) on %4")
                                .arg(str(dlg->frameGeometry()))
                                .arg(dlg->minimumSizeHint().width())
                                .arg(dlg->minimumSizeHint().height())
                                .arg(str(avail))));
    }

    // With a repository open the window still fits: the repository view,
    // the toolbar's branch and filter boxes and the status bar's path (a
    // long one, wider than the screen) ask for less than the screen, so
    // the window isn't pushed past its edge.
    void repositoryWindowFits() {
        // In TestRepo's folder, which keeps libgit2 off the user's config.
        // Capitals make the path wide while it stays well under Windows'
        // MAX_PATH (260), which libgit2 can't init a repository past.
        gitbolt::test::TestRepo outer;
        const QString folder = QStringLiteral("A-WIDE-FOLDER-NAME-IN-CAPITALS");
        const QString path = outer.path() + QStringLiteral("/%1/%1/%1").arg(folder);
        QVERIFY2(path.size() < 200, qPrintable(path));
        QVERIFY(QDir().mkpath(path));
        const auto inited = gitbolt::git::Repository::init(path.toStdString());
        const QString why = inited.ok() ? QString() : QString::fromStdString(inited.error().message());
        QVERIFY2(inited.ok(), qPrintable(why));
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
        const QLabel* pathLabel = nullptr;
        for (const QLabel* label : window.statusBar()->findChildren<QLabel*>()) {
            if (label->text().contains(folder))
                pathLabel = label;
        }
        QVERIFY(pathLabel);
        const int pathWidth = pathLabel->fontMetrics().horizontalAdvance(pathLabel->text());
        QVERIFY2(pathWidth > avail.width(),
                 qPrintable(QStringLiteral("the path is only %1 px wide").arg(pathWidth)));
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

    // The Clone dialog is as large as its two fields need, rather than
    // the big dialogs' 1000 x 800 (all of this screen), and grows to show
    // what a clone says: its progress, then git's error over a few lines.
    void cloneDialogFitsItsContents() {
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        gitbolt::dialogs::CloneDialog dialog(&window);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const QRect avail = window.screen()->availableGeometry();
        QVERIFY2(dialog.height() <= dialog.sizeHint().height(),
                 qPrintable(QStringLiteral("%1 tall, needs %2")
                                .arg(dialog.height()).arg(dialog.sizeHint().height())));
        QVERIFY2(avail.contains(dialog.frameGeometry()), qPrintable(str(dialog.frameGeometry())));
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("clone.url"));
        auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("clone.path"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("clone.status"));
        auto* progress = dialog.findChild<QProgressBar*>();
        QVERIFY(url && path && status && progress);
        QVERIFY2(url->width() >= url->fontMetrics().averageCharWidth() * 50,
                 qPrintable(QString::number(url->width())));
        QVERIFY(allShown(&dialog));

        // A clone git turns down: progress at once, then its error.
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        url->setText(scratch.filePath(QStringLiteral("no-such-repository-%1").arg(QString(40, u'x'))));
        path->setText(QDir::toNativeSeparators(scratch.filePath(QStringLiteral("clone"))));
        QPushButton* clone = nullptr;
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Clone"))
                clone = button;
        }
        QVERIFY(clone);
        clone->click();
        QVERIFY(progress->isVisible() && status->isVisible());
        QTRY_VERIFY(allShown(&dialog));
        QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(), 30000);
        QVERIFY2(status->text().startsWith(QStringLiteral("Clone failed: ")),
                 qPrintable(status->text()));
        QTRY_VERIFY(allShown(&dialog));
        QVERIFY2(avail.contains(dialog.frameGeometry()), qPrintable(str(dialog.frameGeometry())));
    }

    // A message taller than the screen ("Connecting to <url>...", then
    // git's error naming it) is cut short instead: the dialog, its
    // buttons too, stays on the screen.
    void cloneDialogStaysOnTheScreen() {
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        gitbolt::dialogs::CloneDialog dialog(&window);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const QRect avail = window.screen()->availableGeometry();
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("clone.url"));
        auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("clone.path"));
        auto* progress = dialog.findChild<QProgressBar*>();
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(url && path && progress && buttons);

        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        url->setText(QStringLiteral("no such repository ").repeated(200).trimmed());
        path->setText(QDir::toNativeSeparators(scratch.filePath(QStringLiteral("clone"))));
        buttons->button(QDialogButtonBox::Ok)->click();
        QVERIFY(progress->isVisible());
        QVERIFY2(avail.contains(dialog.frameGeometry()), qPrintable(str(dialog.frameGeometry())));
        QVERIFY2(dialog.rect().contains(rectIn(buttons, &dialog)), qPrintable(str(rectIn(buttons, &dialog))));

        QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(), 30000);
        QVERIFY2(avail.contains(dialog.frameGeometry()), qPrintable(str(dialog.frameGeometry())));
        QTRY_VERIFY2(dialog.rect().contains(rectIn(buttons, &dialog)),
                     qPrintable(str(rectIn(buttons, &dialog))));
    }

private:
    // Every visible widget of `dialog` inside it, at its full height: a
    // wrapped label as tall as its text needs.
    static bool allShown(const QWidget* dialog) {
        for (const QWidget* child : dialog->findChildren<QWidget*>()) {
            if (!child->isVisibleTo(dialog) || child->isWindow())
                continue;
            if (!dialog->rect().contains(rectIn(child, dialog))) {
                qWarning("%s at %s is outside %s", child->metaObject()->className(),
                         qPrintable(str(rectIn(child, dialog))), qPrintable(str(dialog->rect())));
                return false;
            }
            const auto* label = qobject_cast<const QLabel*>(child);
            if (label && label->wordWrap() && label->height() < label->heightForWidth(label->width())) {
                qWarning("%s is %d tall, needs %d", qPrintable(label->text()), label->height(),
                         label->heightForWidth(label->width()));
                return false;
            }
        }
        return true;
    }

    // The cards equally tall, and each one's labels (icon, title,
    // description) where the first card's are.
    static bool cardsLineUp(const QWidget* view) {
        QList<QRect> first;
        int height = -1;
        for (const char* name : kCards) {
            const auto* card = view->findChild<QPushButton*>(QString::fromLatin1(name));
            QList<QRect> labels;
            for (const QLabel* label : card->findChildren<QLabel*>())
                labels << QRect(0, label->y(), 1, label->height());
            if (height < 0) {
                height = card->height();
                first = labels;
            } else if (card->height() != height || labels != first) {
                qWarning("%s, %d tall, is out of line with the first card, %d tall", name,
                         card->height(), height);
                for (qsizetype i = 0; i < labels.size() && i < first.size(); ++i)
                    qWarning("  label %lld at y %d, %d tall; first card's at y %d, %d tall",
                             static_cast<long long>(i), labels[i].y(), labels[i].height(),
                             first[i].y(), first[i].height());
                return false;
            }
        }
        return true;
    }

    // The cards whole inside `shown`, apart, and their labels whole, as
    // they'd be at a card's least width too (the row may squeeze it there).
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
                const int margins = card->width() - label->width();
                if (label->minimumSizeHint().width() + margins > card->minimumSizeHint().width()) {
                    qWarning("%s's least width, %d, cuts \"%s\": it needs %d", name,
                             card->minimumSizeHint().width(), qPrintable(label->text()),
                             label->minimumSizeHint().width() + margins);
                    return false;
                }
            }
        }
        return true;
    }
};

QTEST_MAIN(TestSmallScreen)
#include "TestSmallScreen.moc"
