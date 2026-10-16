#include "editor/DiffSyntaxHighlighter.h"

#include "editor/DiffColors.h"

#include <QApplication>
#include <QPalette>

namespace gitbolt::editor {

DiffSyntaxHighlighter::DiffSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    // Foreground-only styling — backgrounds are NOT set here.
    // QTextCharFormat::setBackground only paints under the actual
    // characters, leaving visible white slivers in the line gap
    // between consecutive colored lines (the "double-spaced" look).
    // DiffTextEdit::paintEvent paints each line's background edge
    // to edge instead, the way GitHub's diff renders.

    // File headers (diff --git ...): bold
    fileHeaderFmt_.setFontWeight(QFont::Bold);

    applyColors(diffColorsFor(QApplication::palette()));
}

void DiffSyntaxHighlighter::setColors(const DiffColors& colors)
{
    applyColors(colors);
    rehighlight();
}

void DiffSyntaxHighlighter::applyColors(const DiffColors& colors)
{
    additionFmt_.setForeground(colors.addFg);
    deletionFmt_.setForeground(colors.delFg);
    hunkHeaderFmt_.setForeground(colors.hunkFg);
    contextFmt_.setForeground(colors.contextFg);
}

void DiffSyntaxHighlighter::highlightBlock(const QString& text)
{
    if (text.isEmpty())
        return;

    // A block's text always fits in int: QTextDocument positions (and
    // setFormat()'s arguments) are int.
    const int len = static_cast<int>(text.length());

    if (text.startsWith(QLatin1String("diff --git "))) {
        setFormat(0, len, fileHeaderFmt_);
    } else if (text.startsWith(QLatin1String("@@"))) {
        setFormat(0, len, hunkHeaderFmt_);
    } else if (text.startsWith(QLatin1Char('+'))) {
        // An addition, "+++" and all: the diff shown never has git's
        // "+++ b/" and "--- a/" file headers (DiffViewerWidget renders
        // hunks under its own "diff --git" line), and an added "++i;"
        // or a removed "---" (front matter, an SQL comment) came out
        // as a bold header, as if unchanged.
        setFormat(0, len, additionFmt_);
    } else if (text.startsWith(QLatin1Char('-'))) {
        setFormat(0, len, deletionFmt_);
    } else if (text.startsWith(QLatin1String("index "))
               || text.startsWith(QLatin1String("new file"))
               || text.startsWith(QLatin1String("deleted file"))
               || text.startsWith(QLatin1String("rename"))
               || text.startsWith(QLatin1String("similarity"))) {
        setFormat(0, len, fileHeaderFmt_);
    } else {
        // Context line (starts with space or is unrecognised)
        setFormat(0, len, contextFmt_);
    }
}

} // namespace gitbolt::editor
