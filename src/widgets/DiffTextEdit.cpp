#include "widgets/DiffTextEdit.h"

#include "editor/DiffColors.h"

#include <QApplication>
#include <QPainter>
#include <QPaintEvent>
#include <QTextBlock>
#include <QtMath>

namespace gitbolt::widgets {

namespace {
QColor lineBackground(const QString& text, const editor::DiffColors& colors)
{
    if (text.startsWith(QLatin1String("@@")))
        return colors.hunkBg;
    if (text.startsWith(QLatin1Char('+'))
        && !text.startsWith(QLatin1String("+++")))
        return colors.addBg;
    if (text.startsWith(QLatin1Char('-'))
        && !text.startsWith(QLatin1String("---")))
        return colors.delBg;
    return QColor();
}
} // namespace

DiffTextEdit::DiffTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
}

void DiffTextEdit::paintEvent(QPaintEvent* event)
{
    // Pre-paint full-line-height coloured stripes for every
    // diff-flavoured block in the visible region, then let
    // QPlainTextEdit paint its text on top. Each stripe extends
    // from this block's top to the *next* block's top (or its own
    // bottom for the last block) so any inter-block leading is
    // absorbed by the current stripe. Outward integer rounding
    // (floor top / ceil bottom) makes consecutive stripes overlap
    // by one pixel — eliminating any sub-pixel gap.
    {
        QPainter painter(viewport());
        const int width = viewport()->width();
        QTextBlock block = firstVisibleBlock();
        const QPointF offset = contentOffset();
        // Read at every paint, so a theme switch (which repaints)
        // needs nothing else here.
        const editor::DiffColors colors =
            editor::diffColorsFor(QApplication::palette());

        while (block.isValid()) {
            const QRectF geom = blockBoundingGeometry(block).translated(offset);
            if (geom.top() > viewport()->height())
                break;

            const QColor bg = lineBackground(block.text(), colors);
            if (bg.isValid()) {
                const QTextBlock next = block.next();
                const qreal bottom = next.isValid()
                    ? blockBoundingGeometry(next).translated(offset).top()
                    : geom.bottom();
                const int yTop = qFloor(geom.top());
                const int yBot = qCeil(bottom);
                painter.fillRect(QRect(0, yTop, width, yBot - yTop), bg);
            }
            block = block.next();
        }
    }

    QPlainTextEdit::paintEvent(event);
}

} // namespace gitbolt::widgets
