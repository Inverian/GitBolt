#include "editor/DiffSyntaxHighlighter.h"
#include <QTextCharFormat>

namespace gitbolt::editor {

DiffSyntaxHighlighter::DiffSyntaxHighlighter(QTextDocument* parent) : QSyntaxHighlighter(parent) {}
void DiffSyntaxHighlighter::highlightBlock(const QString& text) {
    QTextCharFormat fmt;
    if (text.startsWith('+')) { fmt.setForeground(QColor(0,170,0)); setFormat(0, text.length(), fmt); }
    else if (text.startsWith('-')) { fmt.setForeground(QColor(204,0,0)); setFormat(0, text.length(), fmt); }
    else if (text.startsWith("@@")) { fmt.setForeground(QColor(0,102,204)); setFormat(0, text.length(), fmt); }
}


} // namespace gitbolt::editor
