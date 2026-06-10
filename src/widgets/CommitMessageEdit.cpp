#include "widgets/CommitMessageEdit.h"

#include <QPainter>
#include <QScrollBar>

namespace gitbolt::widgets {

CommitMessageEdit::CommitMessageEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setPlaceholderText(tr("Enter commit message..."));
    setTabChangesFocus(false);
    setLineWrapMode(QPlainTextEdit::NoWrap);

    QFont monoFont(QStringLiteral("Menlo, Consolas, monospace"));
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(12);
    setFont(monoFont);
}

void CommitMessageEdit::paintEvent(QPaintEvent* event)
{
    // Draw the base widget first
    QPlainTextEdit::paintEvent(event);

    // Paint a vertical guide line at the 72-character column
    QPainter painter(viewport());
    const QFontMetricsF fm(font());
    const qreal charWidth = fm.averageCharWidth();
    const int guideX = static_cast<int>(charWidth * 72.0)
                       - horizontalScrollBar()->value()
                       + static_cast<int>(contentOffset().x());

    painter.setPen(QPen(QColor(200, 200, 200, 128), 1, Qt::DashLine));
    painter.drawLine(guideX, 0, guideX, viewport()->height());
}

} // namespace gitbolt::widgets
