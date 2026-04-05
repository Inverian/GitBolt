#include "editor/DiffSyntaxHighlighter.h"

#include <QColor>

namespace gitbolt::editor {

DiffSyntaxHighlighter::DiffSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    // Addition lines: green foreground, light-green background
    additionFmt_.setForeground(QColor(0, 140, 0));
    additionFmt_.setBackground(QColor(200, 255, 200));

    // Deletion lines: red foreground, light-red background
    deletionFmt_.setForeground(QColor(180, 0, 0));
    deletionFmt_.setBackground(QColor(255, 220, 220));

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

    const int len = text.length();

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
