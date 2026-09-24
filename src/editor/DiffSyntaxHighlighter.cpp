#include "editor/DiffSyntaxHighlighter.h"

#include <QColor>

namespace gitbolt::editor {

DiffSyntaxHighlighter::DiffSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    // Foreground-only styling — backgrounds are NOT set here.
    // QTextCharFormat::setBackground only paints under the actual
    // characters, leaving visible white slivers in the line gap
    // between consecutive colored lines (the "double-spaced" look).
    // The unified-mode renderer applies per-block backgrounds via
    // QTextBlockFormat::setBackground in a separate pass after
    // setPlainText, which paints the entire line height edge-to-
    // edge — matching how the side-by-side renderer already
    // colors its panels and how GitHub's diff renders.

    // Addition lines: green foreground
    additionFmt_.setForeground(QColor(0, 140, 0));

    // Deletion lines: red foreground
    deletionFmt_.setForeground(QColor(180, 0, 0));

    // Hunk headers (@@ ... @@): blue foreground
    hunkHeaderFmt_.setForeground(QColor(0, 102, 204));

    // File headers (diff --git ...): bold
    fileHeaderFmt_.setFontWeight(QFont::Bold);

    // Context lines: gray foreground
    contextFmt_.setForeground(QColor(128, 128, 128));
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
        // Distinguish file-level headers like +++ from additions
        if (text.startsWith(QLatin1String("+++"))) {
            setFormat(0, len, fileHeaderFmt_);
        } else {
            setFormat(0, len, additionFmt_);
        }
    } else if (text.startsWith(QLatin1Char('-'))) {
        if (text.startsWith(QLatin1String("---"))) {
            setFormat(0, len, fileHeaderFmt_);
        } else {
            setFormat(0, len, deletionFmt_);
        }
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
