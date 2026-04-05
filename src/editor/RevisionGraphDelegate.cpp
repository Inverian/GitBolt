#include "editor/RevisionGraphDelegate.h"
#include <QPainter>
#include <QPainterPath>

namespace gitbolt::editor {

const std::array<QColor, 16> RevisionGraphDelegate::laneColors = {{
    {0,153,204}, {204,51,51}, {0,170,85}, {170,85,0},
    {153,51,204}, {204,136,0}, {0,153,153}, {204,0,136},
    {85,170,0}, {0,102,204}, {204,68,68}, {102,102,204},
    {170,136,0}, {0,136,136}, {136,68,170}, {170,102,68}
}};

RevisionGraphDelegate::RevisionGraphDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void RevisionGraphDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());

    auto* model = qobject_cast<const models::CommitLogModel*>(index.model());
    if (!model) { painter->restore(); return; }

    const auto* graphRow = model->graphAt(index.row());
    const auto* commit = model->commitAt(index.row());
    if (!graphRow || !commit) { painter->restore(); return; }

    int x = option.rect.x();
    int y = option.rect.y();
    int h = option.rect.height();

    // Draw lane segments
    for (const auto& seg : graphRow->segments) {
        QPen pen(laneColor(seg.colorIndex), 2.0);
        painter->setPen(pen);
        int fx = x + seg.fromLane * LANE_WIDTH + LANE_WIDTH / 2;
        int tx = x + seg.toLane * LANE_WIDTH + LANE_WIDTH / 2;
        int my = y + h / 2;

        if (seg.type == models::LaneSegmentType::PassThrough) {
            painter->drawLine(fx, y, tx, y + h);
        } else if (fx == tx) {
            painter->drawLine(fx, my, tx, y + h);
        } else {
            QPainterPath path;
            path.moveTo(fx, my);
            path.cubicTo(fx, my + h/3, tx, y + 2*h/3, tx, y + h);
            painter->drawPath(path);
        }
    }

    // Draw commit node
    int cx = x + graphRow->commitLane * LANE_WIDTH + LANE_WIDTH / 2;
    int cy = y + h / 2;
    QColor color = laneColor(graphRow->colorIndex);
    painter->setPen(QPen(color, 1.5));
    painter->setBrush(color);
    if (commit->isMerge()) {
        QPainterPath diamond;
        diamond.moveTo(cx, cy - NODE_RADIUS - 1);
        diamond.lineTo(cx + NODE_RADIUS + 1, cy);
        diamond.lineTo(cx, cy + NODE_RADIUS + 1);
        diamond.lineTo(cx - NODE_RADIUS - 1, cy);
        diamond.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->drawPath(diamond);
    } else {
        painter->drawEllipse(QPoint(cx, cy), NODE_RADIUS, NODE_RADIUS);
    }

    painter->restore();
}

QSize RevisionGraphDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    auto* model = qobject_cast<const models::CommitLogModel*>(index.model());
    int w = 80;
    if (model) {
        const auto* g = model->graphAt(index.row());
        if (g) w = std::max(w, (g->maxLane + 2) * LANE_WIDTH);
    }
    return QSize(w, option.fontMetrics.height() + 4);
}

QColor RevisionGraphDelegate::laneColor(int idx) const {
    return laneColors[static_cast<size_t>(idx) % laneColors.size()];
}

} // namespace gitbolt::editor
