//
// TestDiffColors — the diff viewer, and the other views that coloured
// their text for the light theme only, in both themes:
//
//   - dark: the stripes behind added, deleted and hunk-header lines
//     are dark, and every line reads at 4.5:1 or more on its stripe,
//     in the unified view (highlighter colours) and in the
//     side-by-side panes (plain text, no highlighter);
//   - a theme switch recolours a diff already on screen, highlighter
//     and file name, and a diff opened in the dark theme starts dark;
//   - light: the stripes and text colours it always had;
//   - a removed line starting "--" or an added one starting "++" is
//     striped and coloured as the change it is, not as a file header;
//   - the merge conflict view's headings, the file tree's preview
//     heading and the Clone dialog's status line read at 4.5:1 or
//     more in the dark theme (and the conflict panes' tints follow a
//     switch), and keep their light colours;
//   - so do the Commit tab's labels, links and "Contained in" lines
//     (rendered again on a switch), the Commit dialog's file-status
//     colours, and the toolbar's outcome of an op;
//   - the Commit tab's separators (its message's rule, and the frame
//     over the message) are drawn in the theme's rule colour (#ddd,
//     #3c3c3c), not in the text's (black, near-white) or #ccc;
//   - secondary text (Settings' notes, the Commit dialog's ⓘ, the GPG
//     placeholder, the home screen's cards) reads at 4.5:1 or more in
//     both themes: it was palette(mid), a border's grey;
//   - so do the light theme's own colours of the merge conflict
//     headings, the Commit tab, the Clone status and an op's outcome;
//   - the home screen's card descriptions, and the Submodules and
//     Worktrees windows' status colours, read at 4.5:1 or more in the
//     dark theme too.
//
// Themes switch the way the app switches them, through ThemeService:
// the application palette, then the application style sheet (whose
// repolish puts back a widget's palette from its last polish, which
// is why the views keep these colours in style sheets).
//
// Everything is read from what the widgets render or hold, so this
// builds unchanged against the code before the fix.
//

#include <QApplication>
#include <QDialogButtonBox>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextLayout>

#include <cmath>

#include "conf/SettingsService.h"
#include "conf/ThemeService.h"
#include "dialogs/CloneDialog.h"
#include "dialogs/CommitDialog.h"
#include "dialogs/SettingsDialog.h"
#include "git/Status.h"
#include "models/CommitLogModel.h"
#include "models/FileStatusModel.h"
#include "services/GitService.h"
#include "ui/DashboardView.h"
#include "ui/InlineOpIndicator.h"
#include "ui/RepositoryView.h"
#include "widgets/DiffTextEdit.h"
#include "widgets/DiffViewerWidget.h"
#include "widgets/FileTreeWidget.h"
#include "widgets/MergeConflictWidget.h"
#include "widgets/SubmoduleWidget.h"
#include "widgets/WorktreeWidget.h"

using gitbolt::conf::ThemeService;
using gitbolt::widgets::DiffTextEdit;
using gitbolt::widgets::DiffViewerWidget;

namespace {

// WCAG 2 relative luminance and contrast ratio.
double luminance(const QColor& c) {
    const auto channel = [](int v) {
        const double s = v / 255.0;
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green())
         + 0.0722 * channel(c.blue());
}

double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// `top` over the opaque `bottom`.
QColor over(const QColor& top, const QColor& bottom) {
    const double a = top.alphaF();
    const auto mix = [a](int t, int b) { return static_cast<int>(std::lround(t * a + b * (1 - a))); };
    return QColor(mix(top.red(), bottom.red()), mix(top.green(), bottom.green()),
                  mix(top.blue(), bottom.blue()));
}

QString describe(const QColor& fg, const QColor& bg) {
    return QStringLiteral("%1 on %2: %3:1").arg(fg.name(), bg.name())
        .arg(contrast(fg, bg), 0, 'f', 2);
}

// What `edit` shows at the right end of each of its lines, past any
// text: the line's stripe, or the editor's own background.
QList<QColor> lineBackgrounds(QPlainTextEdit& edit) {
    const QImage image = edit.viewport()->grab().toImage();
    const qreal dpr = image.devicePixelRatio();
    QList<QColor> colors;
    for (QTextBlock block = edit.document()->begin(); block.isValid(); block = block.next()) {
        const int y = edit.cursorRect(QTextCursor(block)).center().y();
        const int x = edit.viewport()->width() - 3;
        colors << image.pixelColor(qRound(x * dpr), qRound(y * dpr));
    }
    return colors;
}

// The highlighter's text colour on each line of `edit`; invalid where
// it set none.
QList<QColor> lineForegrounds(QPlainTextEdit& edit) {
    QList<QColor> colors;
    for (QTextBlock block = edit.document()->begin(); block.isValid(); block = block.next()) {
        const auto formats = block.layout()->formats();
        colors << (formats.isEmpty() || !formats.first().format.hasProperty(QTextFormat::ForegroundBrush)
                       ? QColor()
                       : formats.first().format.foreground().color());
    }
    return colors;
}

// A modified file with one hunk: a context line, a deletion, an
// addition.
gitbolt::git::DiffFileEntry sampleFile() {
    using gitbolt::git::DiffLineType;
    gitbolt::git::DiffFileEntry file;
    file.status = gitbolt::git::DiffStatus::Modified;
    file.oldPath = file.newPath = "a.txt";
    gitbolt::git::DiffHunk hunk;
    hunk.header = "@@ -1,2 +1,2 @@\n";
    hunk.lines = {
        {DiffLineType::Context, "same\n"},
        {DiffLineType::Deletion, "old\n"},
        {DiffLineType::Addition, "new\n"},
    };
    file.hunks = {hunk};
    return file;
}

QPlainTextEdit* unifiedEditor(DiffViewerWidget& viewer) {
    return viewer.findChild<QPlainTextEdit*>(QStringLiteral("diff.unifiedEditor"));
}

// The label in `parent` showing `text`.
QLabel* labelWithText(QWidget& parent, const QString& text) {
    for (QLabel* label : parent.findChildren<QLabel*>()) {
        if (label->text() == text)
            return label;
    }
    return nullptr;
}

QColor textColor(QWidget* widget) {
    widget->ensurePolished();
    return widget->palette().color(QPalette::WindowText);
}

// The colour the browsers of `view` render `text` in (the first
// fragment that contains it).
QColor fragmentColor(QWidget& view, const QString& text) {
    for (QTextBrowser* browser : view.findChildren<QTextBrowser*>()) {
        for (QTextBlock block = browser->document()->begin(); block.isValid();
             block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (fragment.isValid() && fragment.text().contains(text))
                    return fragment.charFormat().foreground().color();
            }
        }
    }
    return {};
}

} // namespace

class TestDiffColors : public QObject {
    Q_OBJECT

    ThemeService theme_;
    QTemporaryDir settings_;
    // What the dark theme's style sheet gives editors and tool bars.
    static constexpr QRgb kDarkEditorBase = 0xff1e1e1e;
    static constexpr QRgb kDarkToolBar = 0xff2d2d2d;

private slots:
    // As main() sets the app up, and the Clone dialog keeps its size in
    // settings: a folder of the test's own, not the user's.
    void initTestCase() {
        QApplication::setStyle(QStringLiteral("fusion"));
        QVERIFY(settings_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }

    void init() { theme_.setTheme(QStringLiteral("Light")); }

    // The diff in the dark theme, both ways it gets there: switched to
    // with a diff on screen, and opened in it.
    void unifiedDiffReadsInTheDarkTheme() {
        DiffViewerWidget switched;
        switched.resize(600, 300);
        switched.setDiffForFile(sampleFile());
        switched.show();
        QVERIFY(QTest::qWaitForWindowExposed(&switched));
        QVERIFY(unifiedEditor(switched));
        const QList<QColor> lightText = lineForegrounds(*unifiedEditor(switched));

        theme_.setTheme(QStringLiteral("Dark"));
        DiffViewerWidget opened;
        opened.resize(600, 300);
        opened.setDiffForFile(sampleFile());
        opened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&opened));

        for (DiffViewerWidget* viewer : {&switched, &opened}) {
            const char* which = viewer == &switched ? "switched to dark" : "opened in dark";
            QPlainTextEdit* edit = unifiedEditor(*viewer);
            QVERIFY(edit);
            const QList<QColor> backgrounds = lineBackgrounds(*edit);
            const QList<QColor> text = lineForegrounds(*edit);
            // diff --git, @@, context, deletion, addition.
            QCOMPARE(backgrounds.size(), 5);
            for (int line : {1, 3, 4}) {
                QVERIFY2(backgrounds[line].lightness() < 100,
                         qPrintable(QStringLiteral("%1: line %2's stripe is %3")
                                        .arg(QLatin1String(which)).arg(line)
                                        .arg(backgrounds[line].name())));
            }
            for (int line : {1, 2, 3, 4}) {
                QVERIFY(text[line].isValid());
                QVERIFY2(contrast(text[line], backgrounds[line]) >= 4.5,
                         qPrintable(QStringLiteral("%1: line %2, %3")
                                        .arg(QLatin1String(which)).arg(line)
                                        .arg(describe(text[line], backgrounds[line]))));
            }
            if (viewer == &switched) {
                QVERIFY2(text[3] != lightText[3], "the switch left the deletion's colour as it was");
            }

            // The file name over it, on the dark toolbar.
            QLabel* name = viewer->findChild<QLabel*>();
            QVERIFY(name);
            QCOMPARE(name->text(), QStringLiteral("a.txt"));
            const QColor nameColor = textColor(name);
            QVERIFY2(contrast(nameColor, QColor(kDarkToolBar)) >= 4.5,
                     qPrintable(QStringLiteral("%1: file name %2").arg(QLatin1String(which))
                                    .arg(describe(nameColor, QColor(kDarkToolBar)))));
        }
    }

    // The side-by-side panes have no highlighter: the theme's own text
    // colour has to read on the stripes.
    void sideBySideReadsInTheDarkTheme() {
        theme_.setTheme(QStringLiteral("Dark"));
        DiffTextEdit edit;
        edit.resize(400, 200);
        edit.setLineWrapMode(QPlainTextEdit::NoWrap);
        edit.setPlainText(QStringLiteral("@@ -1 +1 @@\n-old\n+new"));
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));
        const QColor text = edit.palette().color(QPalette::Text);
        const QList<QColor> backgrounds = lineBackgrounds(edit);
        QCOMPARE(backgrounds.size(), 3);
        for (int line = 0; line < 3; ++line) {
            QVERIFY2(backgrounds[line].lightness() < 100,
                     qPrintable(QStringLiteral("line %1's stripe is %2")
                                    .arg(line).arg(backgrounds[line].name())));
            QVERIFY2(contrast(text, backgrounds[line]) >= 4.5,
                     qPrintable(QStringLiteral("line %1, %2").arg(line)
                                    .arg(describe(text, backgrounds[line]))));
        }
    }

    // The light theme keeps the colours it always had.
    void lightThemeKeepsItsColors() {
        DiffViewerWidget viewer;
        viewer.resize(600, 300);
        viewer.setDiffForFile(sampleFile());
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        QPlainTextEdit* edit = unifiedEditor(viewer);
        QVERIFY(edit);
        const QList<QColor> backgrounds = lineBackgrounds(*edit);
        QCOMPARE(backgrounds.size(), 5);
        QCOMPARE(backgrounds[1], QColor(220, 230, 255));
        QCOMPARE(backgrounds[3], QColor(255, 220, 220));
        QCOMPARE(backgrounds[4], QColor(200, 255, 200));
        const QList<QColor> text = lineForegrounds(*edit);
        QCOMPARE(text[1], QColor(0, 102, 204));
        QCOMPARE(text[2], QColor(128, 128, 128));
        QCOMPARE(text[3], QColor(180, 0, 0));
        QCOMPARE(text[4], QColor(0, 140, 0));
        QLabel* name = viewer.findChild<QLabel*>();
        QVERIFY(name);
        QCOMPARE(textColor(name), QColor(0x55, 0x55, 0x55));

        // And gets them back from the dark theme.
        theme_.setTheme(QStringLiteral("Dark"));
        theme_.setTheme(QStringLiteral("Light"));
        QCOMPARE(lineBackgrounds(*edit)[3], QColor(255, 220, 220));
        QCOMPARE(lineForegrounds(*edit)[3], QColor(180, 0, 0));
        QCOMPARE(textColor(name), QColor(0x55, 0x55, 0x55));
    }

    // A removed line that starts with "--" (Markdown's or YAML's "---"
    // front matter, an SQL or Lua comment) or an added one that starts
    // with "++" ("++i;") is a change like any other: its stripe and
    // colour. They were told from git's "--- a/" and "+++ b/" file
    // headers, which these views never show, by their first three
    // characters, and came out bold and unstriped, as if unchanged.
    void linesThatLookLikeFileHeadersAreChanges() {
        using gitbolt::git::DiffLineType;
        gitbolt::git::DiffFileEntry file;
        file.status = gitbolt::git::DiffStatus::Modified;
        file.oldPath = file.newPath = "post.md";
        gitbolt::git::DiffHunk hunk;
        hunk.header = "@@ -1,4 +1,2 @@\n";
        hunk.lines = {
            {DiffLineType::Deletion, "---\n"},
            {DiffLineType::Deletion, "-- sql comment\n"},
            {DiffLineType::Addition, "++i;\n"},
            {DiffLineType::Addition, "+\n"},
        };
        file.hunks = {hunk};
        DiffViewerWidget viewer;
        viewer.resize(600, 300);
        viewer.setDiffForFile(file);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        QPlainTextEdit* edit = unifiedEditor(viewer);
        QVERIFY(edit);
        // diff --git, @@, "----", "--- sql comment", "+++i;", "++".
        const QList<QColor> backgrounds = lineBackgrounds(*edit);
        const QList<QColor> text = lineForegrounds(*edit);
        QCOMPARE(backgrounds.size(), 6);
        QCOMPARE(edit->document()->findBlockByNumber(2).text(), QStringLiteral("----"));
        QCOMPARE(edit->document()->findBlockByNumber(4).text(), QStringLiteral("+++i;"));
        for (int line : {2, 3}) {
            QCOMPARE(backgrounds[line], QColor(255, 220, 220));
            QCOMPARE(text[line], QColor(180, 0, 0));
        }
        for (int line : {4, 5}) {
            QCOMPARE(backgrounds[line], QColor(200, 255, 200));
            QCOMPARE(text[line], QColor(0, 140, 0));
        }

        // The side-by-side panes' stripes too.
        DiffTextEdit pane;
        pane.resize(400, 200);
        pane.setLineWrapMode(QPlainTextEdit::NoWrap);
        pane.setPlainText(QStringLiteral("----\n+++i;"));
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));
        const QList<QColor> stripes = lineBackgrounds(pane);
        QCOMPARE(stripes.size(), 2);
        QCOMPARE(stripes[0], QColor(255, 220, 220));
        QCOMPARE(stripes[1], QColor(200, 255, 200));
    }

    void mergeConflictViewReadsInTheDarkTheme() {
        gitbolt::widgets::MergeConflictWidget widget;
        gitbolt::git::MergeConflictEntry conflict;
        conflict.path = "a.txt";
        conflict.ancestorContent = "base\n";
        conflict.oursContent = "ours\n";
        conflict.theirsContent = "theirs\n";
        widget.setConflicts({conflict});
        widget.resize(800, 500);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        const QStringList headings = {
            gitbolt::widgets::MergeConflictWidget::tr("Ours (Local)"),
            gitbolt::widgets::MergeConflictWidget::tr("Base (Common Ancestor)"),
            gitbolt::widgets::MergeConflictWidget::tr("Theirs (Remote)"),
            gitbolt::widgets::MergeConflictWidget::tr("Merged Result (editable)"),
        };
        QList<QColor> lightHeadings;
        for (const QString& heading : headings) {
            QLabel* label = labelWithText(widget, heading);
            QVERIFY2(label, qPrintable(heading));
            lightHeadings << textColor(label);
        }
        // At 4.5:1 or more on the light window too: royal blue, gray and
        // forest green read at 3.4:1 to 4.2:1 there.
        const QColor lightWindow = QApplication::palette().color(QPalette::Window);
        for (int i = 0; i < lightHeadings.size(); ++i) {
            QVERIFY2(contrast(lightHeadings[i], lightWindow) >= 4.5,
                     qPrintable(headings[i] + QStringLiteral(": ")
                                + describe(lightHeadings[i], lightWindow)));
        }
        QCOMPARE(lightHeadings.last(), QColor(0x8b, 0x45, 0x13));
        // The three read-only panes: ours, base, theirs.
        QList<QPlainTextEdit*> panes;
        for (QPlainTextEdit* edit : widget.findChildren<QPlainTextEdit*>()) {
            if (edit->isReadOnly())
                panes << edit;
        }
        QCOMPARE(panes.size(), 3);
        // The background of the pane's first character.
        const auto tintOf = [](QPlainTextEdit* edit) {
            QTextCursor cursor(edit->document());
            cursor.movePosition(QTextCursor::NextCharacter);
            return cursor.charFormat().background().color();
        };
        QList<QColor> lightTints;
        for (QPlainTextEdit* pane : panes)
            lightTints << tintOf(pane);

        theme_.setTheme(QStringLiteral("Dark"));
        const QColor window = QApplication::palette().color(QPalette::Window);
        for (const QString& heading : headings) {
            const QColor color = textColor(labelWithText(widget, heading));
            QVERIFY2(contrast(color, window) >= 4.5,
                     qPrintable(heading + QStringLiteral(": ") + describe(color, window)));
        }
        for (int i = 0; i < panes.size(); ++i) {
            const QColor tint = tintOf(panes[i]);
            QVERIFY2(tint != lightTints[i], "the switch left a pane's tint as it was");
            const QColor behind = over(tint, QColor(kDarkEditorBase));
            const QColor text = panes[i]->palette().color(QPalette::Text);
            QVERIFY2(contrast(text, behind) >= 4.5, qPrintable(describe(text, behind)));
        }
    }

    void fileTreeHeadingReadsInTheDarkTheme() {
        gitbolt::widgets::FileTreeWidget tree;
        tree.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tree));
        QLabel* heading = labelWithText(
            tree, gitbolt::widgets::FileTreeWidget::tr("Select a file to preview its contents"));
        QVERIFY(heading);
        QCOMPARE(textColor(heading), QColor(0x44, 0x44, 0x44));

        theme_.setTheme(QStringLiteral("Dark"));
        const QColor window = QApplication::palette().color(QPalette::Window);
        const QColor color = textColor(heading);
        QVERIFY2(contrast(color, window) >= 4.5, qPrintable(describe(color, window)));
    }

    // The Commit tab: label greys, links and the "Contained in" lines
    // were the light theme's (1.7:1 to 3.6:1 on the dark one), and a
    // theme switch renders the commit on show again.
    void commitDetailsReadInTheDarkTheme() {
        gitbolt::models::CommitLogModel model;
        gitbolt::git::CommitData commit;
        commit.id = gitbolt::git::ObjectId::fromHex("0123456789abcdef0123456789abcdef01234567");
        commit.author.name = commit.committer.name = "Ada";
        commit.author.email = commit.committer.email = "ada@example.com";
        commit.summary = commit.message = "A commit";
        model.setCommits({commit});
        gitbolt::ui::RepositoryView view;
        view.setCommitLogModel(&model);
        view.resize(900, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.onCommitSelected(QString::fromStdString(commit.id.toHex()));
        const QStringList texts = {QStringLiteral("Author:"), QStringLiteral("ada@example.com"),
                                   QStringLiteral("Contained in no tag"),
                                   QStringLiteral("(none — root commit)")};
        QCOMPARE(fragmentColor(view, QStringLiteral("Author:")), QColor(0x66, 0x66, 0x66));
        QCOMPARE(fragmentColor(view, QStringLiteral("Contained in no tag")),
                 QColor(0x44, 0x44, 0x44));
        // Every one at 4.5:1 or more on the light window the browsers
        // show through, the link ("ada@example.com") and the muted
        // "(none — root commit)" too (#1f6feb and #888: 4.0:1, 3.1:1).
        {
            const QColor base = QApplication::palette().color(QPalette::Window);
            for (const QString& text : texts) {
                const QColor color = fragmentColor(view, text);
                QVERIFY2(color.isValid(), qPrintable(text));
                QVERIFY2(contrast(color, base) >= 4.5,
                         qPrintable(text + QStringLiteral(": ") + describe(color, base)));
            }
        }

        // Switched to with the commit on show, and shown afresh.
        theme_.setTheme(QStringLiteral("Dark"));
        for (int pass = 0; pass < 2; ++pass) {
            // The browsers are transparent over the panel's window colour.
            const QColor base = QApplication::palette().color(QPalette::Window);
            for (const QString& text : texts) {
                const QColor color = fragmentColor(view, text);
                QVERIFY2(color.isValid(), qPrintable(text));
                QVERIFY2(contrast(color, base) >= 4.5,
                         qPrintable(text + QStringLiteral(": ") + describe(color, base)));
            }
            view.onCommitSelected(QString::fromStdString(commit.id.toHex()));
        }
    }

    // The Commit dialog's file lists: modified (blue), deleted and
    // conflicted (red), new (green, gray) at 4.5:1 or more on the dark
    // theme's list background; the light colours as they were.
    void fileStatusColorsReadInTheDarkTheme() {
        using FS = gitbolt::git::FileStatus;
        std::vector<gitbolt::git::StatusEntry> entries = {
            {"modified.txt", "", FS::WtModified | FS::IndexModified},
            {"deleted.txt", "", FS::WtDeleted | FS::IndexDeleted},
            {"new.txt", "", FS::WtNew},
            {"added.txt", "", FS::IndexNew},
            {"conflicted.txt", "", FS::Conflicted},
        };
        gitbolt::models::FileStatusModel unstaged;
        unstaged.setEntries(entries);
        gitbolt::models::FileStatusModel staged;
        staged.setStagedFilter(true);
        staged.setEntries(entries);
        const auto colors = [](const gitbolt::models::FileStatusModel& model) {
            QList<QColor> list;
            for (int row = 0; row < model.rowCount(); ++row)
                list << model.data(model.index(row, gitbolt::models::FileStatusModel::Path),
                                   Qt::ForegroundRole).value<QColor>();
            return list;
        };
        QVERIFY(colors(unstaged).contains(QColor(0, 102, 204)));
        QVERIFY(colors(unstaged).contains(QColor(204, 0, 0)));

        theme_.setTheme(QStringLiteral("Dark"));
        // The lists alternate their rows' colours.
        for (const QColor& base : {QApplication::palette().color(QPalette::Base),
                                   QApplication::palette().color(QPalette::AlternateBase)}) {
            for (const auto* model : {&unstaged, &staged}) {
                const QList<QColor> list = colors(*model);
                QVERIFY(!list.isEmpty());
                for (const QColor& color : list)
                    QVERIFY2(contrast(color, base) >= 4.5, qPrintable(describe(color, base)));
            }
        }
    }

    // The Commit tab's separator: its colour was set as a border, which
    // Qt's <hr> ignores, so it was drawn in the text colour, near-white
    // on the dark theme and black on the light one. It takes the rule
    // colour now (#ddd, #3c3c3c), switched with the theme.
    void commitSeparatorTakesTheThemesColour() {
        gitbolt::models::CommitLogModel model;
        gitbolt::git::CommitData commit;
        commit.id = gitbolt::git::ObjectId::fromHex("0123456789abcdef0123456789abcdef01234567");
        commit.author.name = commit.committer.name = "Ada";
        commit.author.email = commit.committer.email = "ada@example.com";
        commit.summary = commit.message = "A commit";
        model.setCommits({commit});
        gitbolt::ui::RepositoryView view;
        view.setCommitLogModel(&model);
        view.resize(900, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.onCommitSelected(QString::fromStdString(commit.id.toHex()));
        QTextBrowser* message = nullptr;
        for (QTextBrowser* browser : view.findChildren<QTextBrowser*>()) {
            if (browser->toPlainText().contains(QStringLiteral("Contained in no tag")))
                message = browser;
        }
        QVERIFY(message);
        // A row of the rendered browser more than half of whose pixels
        // are `rule`: the separator, drawn across the page.
        const auto drawsRule = [&](const QColor& rule) {
            QApplication::processEvents();
            const QImage image = message->viewport()->grab().toImage();
            for (int y = 0; y < image.height(); ++y) {
                int matching = 0;
                for (int x = 0; x < image.width(); ++x) {
                    if (QColor(image.pixel(x, y)).rgb() == rule.rgb())
                        ++matching;
                }
                if (matching > image.width() / 2)
                    return true;
            }
            return false;
        };
        // The full-width frame between the details and the message: it
        // stayed #ccc, a bright line across the dark theme's panel.
        auto* frame = view.findChild<QFrame*>(QStringLiteral("commit.separator"));
        if (!frame) {
            for (QFrame* candidate : view.findChildren<QFrame*>()) {
                if (candidate->frameShape() == QFrame::HLine)
                    frame = candidate;
            }
        }
        QVERIFY(frame);
        const auto frameColor = [frame] {
            QApplication::processEvents();
            const QImage image = frame->grab().toImage();
            return image.pixelColor(image.width() / 2, image.height() / 2);
        };
        QVERIFY2(drawsRule(QColor(0xdd, 0xdd, 0xdd)), "no #ddd rule in the light theme");
        QCOMPARE(frameColor(), QColor(0xdd, 0xdd, 0xdd));
        theme_.setTheme(QStringLiteral("Dark"));
        QVERIFY2(drawsRule(QColor(0x3c, 0x3c, 0x3c)), "no #3c3c3c rule in the dark theme");
        QCOMPARE(frameColor(), QColor(0x3c, 0x3c, 0x3c));
        theme_.setTheme(QStringLiteral("Light"));
        QCOMPARE(frameColor(), QColor(0xdd, 0xdd, 0xdd));
    }

    // Secondary text was palette(mid), a border's grey: 1.7:1 on the
    // light theme's window and 2.1:1 on the dark one's. Settings' notes
    // (the usage statistics' among them), the Commit dialog's ⓘ, the
    // Commit tab's GPG placeholder and the home screen's card text read
    // at 4.5:1 or more now, in both themes and across a switch.
    void secondaryTextReadsInBothThemes() {
        gitbolt::conf::SettingsService settings;
        gitbolt::dialogs::SettingsDialog settingsDialog(&settings, nullptr);
        settingsDialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&settingsDialog));
        gitbolt::services::GitService svc;
        gitbolt::dialogs::CommitDialog commitDialog(&svc, &settings);
        commitDialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&commitDialog));
        gitbolt::ui::RepositoryView view;
        view.resize(900, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        gitbolt::ui::DashboardView home(&settings);
        home.resize(1100, 800);
        home.show();
        QVERIFY(QTest::qWaitForWindowExposed(&home));

        QList<QLabel*> labels;
        for (QLabel* label : settingsDialog.findChildren<QLabel*>()) {
            // Text in the dialog's own colours (not a link's, nor an
            // image's), shown or shown once the page is.
            if (!label->text().isEmpty() && label->textFormat() != Qt::RichText
                && !label->text().contains(QLatin1Char('<')))
                labels << label;
        }
        QVERIFY(labels.size() > 5);
        QLabel* info = labelWithText(commitDialog, QStringLiteral("ⓘ"));
        QVERIFY(info);
        labels << info;
        QLabel* gpg = labelWithText(
            view, gitbolt::ui::RepositoryView::tr("GPG signature verification — coming soon"));
        QVERIFY(gpg);
        labels << gpg;
        QLabel* card = labelWithText(
            home, gitbolt::ui::DashboardView::tr("Open an existing local repository"));
        QVERIFY(card);

        for (const char* themeName : {"Light", "Dark", "Light"}) {
            theme_.setTheme(QString::fromLatin1(themeName));
            const QColor window = QApplication::palette().color(QPalette::Window);
            for (QLabel* label : std::as_const(labels)) {
                const QColor color = textColor(label);
                QVERIFY2(contrast(color, window) >= 4.5,
                         qPrintable(QStringLiteral("%1: \"%2\": %3")
                                        .arg(QLatin1String(themeName),
                                             label->text().left(40),
                                             describe(color, window))));
            }
        }
        // The cards' own surface in the light theme is the window's.
        const QColor window = QApplication::palette().color(QPalette::Window);
        QVERIFY2(contrast(textColor(card), window) >= 4.5,
                 qPrintable(describe(textColor(card), window)));
    }

    // The home screen's cards: their descriptions were palette(mid),
    // the light theme's grey, which read at 1.4:1 on the dark theme's
    // cards. Switched to with the home screen up.
    void homeScreenCardsReadInTheDarkTheme() {
        gitbolt::conf::SettingsService settings;
        gitbolt::ui::DashboardView view(&settings);
        view.resize(1100, 800);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        theme_.setTheme(QStringLiteral("Dark"));
        // A card, and a hovered one (palette(midlight)).
        const QColor card(0x3c, 0x3c, 0x3c);
        const QColor hovered = QApplication::palette().color(QPalette::Midlight);
        for (const QString& text : {gitbolt::ui::DashboardView::tr("Open an existing local repository"),
                                    gitbolt::ui::DashboardView::tr("Clone a remote repository"),
                                    gitbolt::ui::DashboardView::tr("Create a new empty repository")}) {
            QLabel* label = labelWithText(view, text);
            QVERIFY2(label, qPrintable(text));
            const QColor color = textColor(label);
            for (const QColor& bg : {card, hovered})
                QVERIFY2(contrast(color, bg) >= 4.5,
                         qPrintable(text + QStringLiteral(": ") + describe(color, bg)));
        }
    }

    // The Submodules and Worktrees windows' status colours, on the dark
    // theme's rows (#191919, and #252525 every other row); the light
    // theme's as they were.
    void submoduleAndWorktreeStatusReadInTheDarkTheme() {
        using Status = gitbolt::git::SubmoduleStatus;
        std::vector<gitbolt::git::SubmoduleInfo> subs;
        for (const Status status : {Status::Clean, Status::Dirty, Status::Modified,
                                    Status::Uninitialized, Status::OutOfDate, Status::Added,
                                    Status::Deleted}) {
            gitbolt::git::SubmoduleInfo info;
            info.name = info.path = "sub";
            info.status = status;
            subs.push_back(info);
        }
        gitbolt::widgets::SubmoduleTableModel submodules;
        submodules.setSubmodules(subs);
        gitbolt::widgets::WorktreeTableModel worktrees;
        gitbolt::git::WorktreeInfo locked;
        locked.name = locked.path = "wt";
        locked.isLocked = true;
        gitbolt::git::WorktreeInfo unlocked = locked;
        unlocked.isLocked = false;
        worktrees.setWorktrees({locked, unlocked});
        const auto colors = [&] {
            QList<QColor> list;
            for (int row = 0; row < submodules.rowCount(); ++row)
                list << submodules.data(submodules.index(
                            row, gitbolt::widgets::SubmoduleTableModel::ColStatus),
                                        Qt::ForegroundRole).value<QColor>();
            for (int row = 0; row < worktrees.rowCount(); ++row)
                list << worktrees.data(worktrees.index(
                            row, gitbolt::widgets::WorktreeTableModel::ColLocked),
                                       Qt::ForegroundRole).value<QColor>();
            return list;
        };
        QCOMPARE(colors().first(), QColor(0x2e, 0x7d, 0x32));   // Clean, light

        theme_.setTheme(QStringLiteral("Dark"));
        const QList<QColor> dark = colors();
        QCOMPARE(dark.size(), 9);
        for (const QColor& base : {QApplication::palette().color(QPalette::Base),
                                   QColor(0x25, 0x25, 0x25)}) {
            for (const QColor& color : dark) {
                QVERIFY(color.isValid());
                QVERIFY2(contrast(color, base) >= 4.5, qPrintable(describe(color, base)));
            }
        }
    }

    // The toolbar's word on an op's outcome, on the dark tool bar.
    void opOutcomeReadsInTheDarkTheme() {
        theme_.setTheme(QStringLiteral("Dark"));
        QWidget bar;
        auto* label = new QLabel(&bar);
        gitbolt::ui::InlineOpIndicator indicator(label);
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        const QColor toolBar = QColor::fromRgb(kDarkToolBar);
        indicator.fail(QStringLiteral("Push failed"), QStringLiteral("rejected"), 0);
        QColor color = textColor(label);
        QVERIFY2(contrast(color, toolBar) >= 4.5, qPrintable(describe(color, toolBar)));
        indicator.succeed(QStringLiteral("Pushed"), 0);
        color = textColor(label);
        QVERIFY2(contrast(color, toolBar) >= 4.5, qPrintable(describe(color, toolBar)));

        // And the light theme's on its own tool bar (the window's
        // colour): its green, #2e9c36, read at 3.1:1.
        theme_.setTheme(QStringLiteral("Light"));
        const QColor lightBar = QApplication::palette().color(QPalette::Window);
        indicator.succeed(QStringLiteral("Pushed"), 0);
        color = textColor(label);
        QVERIFY2(contrast(color, lightBar) >= 4.5, qPrintable(describe(color, lightBar)));
        indicator.fail(QStringLiteral("Push failed"), QStringLiteral("rejected"), 0);
        color = textColor(label);
        QVERIFY2(contrast(color, lightBar) >= 4.5, qPrintable(describe(color, lightBar)));
    }

    // A remote op's failure stays on the label until the next op, so it
    // must follow a theme switch. Its red was picked once, for the theme
    // of the moment, and kept in the label's own style sheet: after a
    // switch it read at 2.7:1 on the dark tool bar, and at 2.2:1 on the
    // light one the other way round.
    void stickyOpFailureFollowsAThemeSwitch() {
        QWidget bar;
        auto* label = new QLabel(&bar);
        gitbolt::ui::InlineOpIndicator indicator(label);
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        indicator.fail(QStringLiteral("fetch failed: no route"), QStringLiteral("no route"), 0);
        const QColor lightFailure = textColor(label);

        theme_.setTheme(QStringLiteral("Dark"));
        const QColor darkBar = QColor::fromRgb(kDarkToolBar);
        QColor color = textColor(label);
        QVERIFY2(contrast(color, darkBar) >= 4.5, qPrintable(describe(color, darkBar)));
        QVERIFY(label->text().startsWith(QStringLiteral("✗ ")));

        theme_.setTheme(QStringLiteral("Light"));
        color = textColor(label);
        QCOMPARE(color, lightFailure);
        const QColor lightBar = QApplication::palette().color(QPalette::Window);
        QVERIFY2(contrast(color, lightBar) >= 4.5, qPrintable(describe(color, lightBar)));

        // A success, shown until it clears, the same way.
        indicator.succeed(QStringLiteral("Pushed"), 0);
        theme_.setTheme(QStringLiteral("Dark"));
        color = textColor(label);
        QVERIFY2(contrast(color, darkBar) >= 4.5, qPrintable(describe(color, darkBar)));
    }

    // The status line's error colour (Clone with no URL), in both
    // themes, and across a switch while it shows.
    void cloneStatusReadsInTheDarkTheme() {
        gitbolt::dialogs::CloneDialog dialog;
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Ok)->click();
        const QString message = gitbolt::dialogs::CloneDialog::tr("Please enter a repository URL.");
        QLabel* status = labelWithText(dialog, message);
        QVERIFY(status);
        QVERIFY(status->isVisible());
        // At 4.5:1 on the light window too (its #d73a49 read at 4.0:1).
        {
            const QColor window = QApplication::palette().color(QPalette::Window);
            const QColor color = textColor(status);
            QVERIFY2(contrast(color, window) >= 4.5, qPrintable(describe(color, window)));
        }

        theme_.setTheme(QStringLiteral("Dark"));
        const QColor window = QApplication::palette().color(QPalette::Window);
        QColor color = textColor(status);
        QVERIFY2(contrast(color, window) >= 4.5, qPrintable(describe(color, window)));

        // And shown afresh in the dark theme.
        buttons->button(QDialogButtonBox::Ok)->click();
        color = textColor(status);
        QVERIFY2(contrast(color, window) >= 4.5, qPrintable(describe(color, window)));
    }
};

QTEST_MAIN(TestDiffColors)
#include "TestDiffColors.moc"
