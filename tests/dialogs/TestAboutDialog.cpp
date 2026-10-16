//
// TestAboutDialog — Help › About is never too short for what it shows.
//
// A top-level window's minimum size ignores height-for-width, so the
// dialog's minimum height came out about 45 px short of what its
// word-wrapped labels need at its minimum width. Dragged that small,
// the build-info table was clipped; and on a short screen it opened
// clipped, because Qt caps a new window at 2/3 of the screen's height
// and only the minimum bounds that cap.
//
// Runs off screen on a 1093x614 screen (tests/fixtures/short-screen.json:
// 1366x768 at 125 %, a common laptop) with the app's .qrc linked, so the
// dialog shows its real 128 px icon.
//

#include <QGuiApplication>
#include <QLabel>
#include <QLayout>
#include <QScreen>
#include <QTest>

#include "dialogs/AboutDialog.h"

using gitbolt::dialogs::AboutDialog;

namespace {

// The labels of `dlg` shorter than they need to be at their current
// width: wrapping labels need their height-for-width, the rest their
// size hint. Empty when nothing is clipped.
QStringList clippedLabels(const AboutDialog& dlg) {
    QStringList clipped;
    for (const QLabel* label : dlg.findChildren<QLabel*>()) {
        const int needs = label->hasHeightForWidth()
            ? label->heightForWidth(label->width())
            : label->sizeHint().height();
        if (label->height() < needs) {
            clipped << QStringLiteral("%1 px of %2 for \"%3\"")
                           .arg(label->height()).arg(needs)
                           .arg(label->text().left(60));
        }
    }
    return clipped;
}

} // namespace

class TestAboutDialog : public QObject {
    Q_OBJECT

private slots:
    void minimumHeightCoversTheWrappedText() {
        AboutDialog dlg;
        QVERIFY(dlg.layout());
        // What show() does first: the layout sets the minimum size it
        // computes wherever the dialog set none of its own.
        dlg.layout()->activate();
        const int needed = dlg.layout()->totalHeightForWidth(dlg.minimumWidth());
        QVERIFY2(dlg.minimumHeight() >= needed,
                 qPrintable(QStringLiteral("minimum height %1 < %2, what the "
                                           "labels need at the minimum width %3")
                                .arg(dlg.minimumHeight()).arg(needed)
                                .arg(dlg.minimumWidth())));
        // A minimum, not a fixed size: the dialog stays resizable.
        QVERIFY(dlg.maximumHeight() > dlg.minimumHeight());
        QVERIFY(dlg.maximumWidth() > dlg.minimumWidth());
    }

    void smallestSizeClipsNoLabel() {
        AboutDialog dlg;
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        // As small as it goes: resize() stops at the minimum.
        dlg.resize(dlg.minimumWidth(), 1);
        QTRY_COMPARE(dlg.height(), dlg.minimumHeight());
        const QStringList clipped = clippedLabels(dlg);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join(QStringLiteral("; "))));
    }

    void opensUnclippedOnAShortScreen() {
        // The fixture's screen, or this proves nothing: on a tall one
        // the 2/3 cap never bites.
        const QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        QCOMPARE(screen->geometry().size(), QSize(1093, 614));

        AboutDialog dlg;
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QVERIFY2(dlg.height() <= screen->availableGeometry().height(),
                 "the dialog no longer fits the screen; the fixture is too short");
        const QStringList clipped = clippedLabels(dlg);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join(QStringLiteral("; "))));
    }
};

QTEST_MAIN(TestAboutDialog)
#include "TestAboutDialog.moc"
