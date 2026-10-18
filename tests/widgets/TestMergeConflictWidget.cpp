//
// TestMergeConflictWidget — the conflict resolver's panes for a
// conflict on a symbolic link. A link's content is the path it points
// to, and the panes showed it as a one-line file with nothing to tell
// it apart: the result was editable, and an edit or Accept Both made a
// "resolution" that was neither side. Now the headings say which sides
// are links, and the result can only be a side as it is (read-only, no
// Accept Both); a text conflict next to it is editable as ever.
//

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTest>

#include "widgets/MergeConflictWidget.h"

using gitbolt::git::MergeConflictEntry;
using gitbolt::widgets::MergeConflictWidget;

namespace {

QStringList headings(MergeConflictWidget& widget) {
    QStringList texts;
    for (QLabel* label : widget.findChildren<QLabel*>())
        texts << label->text();
    return texts;
}

QPushButton* button(MergeConflictWidget& widget, const QString& text) {
    for (QPushButton* b : widget.findChildren<QPushButton*>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

MergeConflictEntry conflict(const char* path, const char* ours, const char* theirs,
                            uint32_t mode) {
    MergeConflictEntry entry;
    entry.path = path;
    entry.ancestorContent = "base";
    entry.oursContent = ours;
    entry.theirsContent = theirs;
    entry.ancestorMode = entry.oursMode = entry.theirsMode = mode;
    return entry;
}

} // namespace

class TestMergeConflictWidget : public QObject {
    Q_OBJECT

private slots:
    void linkConflictIsMarkedAndResolvedByASide() {
        MergeConflictWidget widget;
        widget.setConflicts({conflict("link", "/outside/MARKER", "b",
                                      MergeConflictEntry::kSymlinkMode),
                             conflict("a.txt", "ours\n", "theirs\n", 0100644)});
        auto* result = widget.findChild<QPlainTextEdit*>(QStringLiteral("conflict.result"));
        QPushButton* acceptTheirs = button(widget, MergeConflictWidget::tr("Accept Theirs"));
        QPushButton* acceptBoth = button(widget, MergeConflictWidget::tr("Accept Both"));
        QPushButton* next = button(widget, MergeConflictWidget::tr("Next >>"));
        QPushButton* prev = button(widget, MergeConflictWidget::tr("<< Prev"));
        QVERIFY(result && acceptTheirs && acceptBoth && next && prev);

        const QStringList linkHeadings = {
            MergeConflictWidget::tr("Ours (Local), a symbolic link"),
            MergeConflictWidget::tr("Base (Common Ancestor), a symbolic link"),
            MergeConflictWidget::tr("Theirs (Remote), a symbolic link"),
            MergeConflictWidget::tr("Merged Result (a symbolic link: accept Ours or Theirs)"),
        };
        for (const QString& heading : linkHeadings)
            QVERIFY2(headings(widget).contains(heading), qPrintable(heading));
        QVERIFY(result->isReadOnly());
        QVERIFY(!acceptBoth->isEnabled());
        QCOMPARE(result->toPlainText(), QStringLiteral("/outside/MARKER"));
        acceptTheirs->click();
        QCOMPARE(result->toPlainText(), QStringLiteral("b"));

        // The text conflict: plain headings, editable, Accept Both.
        next->click();
        const QStringList textHeadings = {
            MergeConflictWidget::tr("Ours (Local)"),
            MergeConflictWidget::tr("Base (Common Ancestor)"),
            MergeConflictWidget::tr("Theirs (Remote)"),
            MergeConflictWidget::tr("Merged Result (editable)"),
        };
        for (const QString& heading : textHeadings)
            QVERIFY2(headings(widget).contains(heading), qPrintable(heading));
        QVERIFY(!result->isReadOnly());
        QVERIFY(acceptBoth->isEnabled());
        acceptBoth->click();
        QCOMPARE(result->toPlainText(), QStringLiteral("ours\ntheirs\n"));

        // Back at the link, theirs is still the result, and still a side.
        prev->click();
        QVERIFY(result->isReadOnly());
        QVERIFY(!acceptBoth->isEnabled());
        QCOMPARE(result->toPlainText(), QStringLiteral("b"));
        QPushButton* markAll = button(widget, MergeConflictWidget::tr("Mark All Resolved"));
        QVERIFY(markAll);
        markAll->click();
        QCOMPARE(widget.allResolvedContents(),
                 (std::vector<QString>{QStringLiteral("b"), QStringLiteral("ours\ntheirs\n")}));
    }
};

QTEST_MAIN(TestMergeConflictWidget)
#include "TestMergeConflictWidget.moc"
