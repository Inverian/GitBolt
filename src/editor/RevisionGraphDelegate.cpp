#include "editor/RevisionGraphDelegate.h"
#include <QAbstractProxyModel>
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
    // Backstop: bezier merge curves and pass-through verticals from
    // a wider region must never paint over the Message column while
    // the fixed column width is catching up.
    painter->setClipRect(option.rect);
    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());

    // The view may have a QSortFilterProxyModel (or any other proxy)
    // sitting between the table and our CommitLogModel — that's how
    // the toolbar Filter input applies to the grid. Walk through any
    // chained proxies to find the real CommitLogModel and the
    // matching source row before we look up graph + commit data.
    const QAbstractItemModel* m = index.model();
    QModelIndex idx = index;
    while (auto* proxy = qobject_cast<const QAbstractProxyModel*>(m)) {
        idx = proxy->mapToSource(idx);
        m = proxy->sourceModel();
    }
    auto* model = qobject_cast<const models::CommitLogModel*>(m);
    if (!model || !idx.isValid()) { painter->restore(); return; }

    const auto* graphRow = model->graphAt(idx.row());
    const auto* commit = model->commitAt(idx.row());
    if (!graphRow || !commit) { painter->restore(); return; }

    int x = option.rect.x();
    int y = option.rect.y();
    int h = option.rect.height();
    int my = y + h / 2;

    // ---- Lane segments ------------------------------------------
    // Each row is conceptually split in half at the commit dot:
    //   y .... my   (top half — incoming lines)
    //   my ... y+h  (bottom half — outgoing lines)
    //
    // Segment kinds:
    //   PassThrough — full vertical, top to bottom (no commit on
    //                 this lane in this row)
    //   End         — top half on the commit's own lane (the line
    //                 coming down from above into the dot)
    //   Start       — bottom half on the commit's own lane (the
    //                 line going down to the first parent)
    //   MergeRight  — bottom-half S-curve from commit lane to an
    //                 already-active lane to the right (this is a
    //                 merge whose other parent is on that lane)
    //   SplitRight  — bottom-half S-curve from commit lane to a
    //                 brand new lane to the right (a merge that
    //                 introduces a new branch)
    //
    // The "Left" variants are reserved for future use.
    for (const auto& seg : graphRow->segments) {
        QPen pen(laneColor(seg.colorIndex), 2.0);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        const int fx = x + seg.fromLane * LANE_WIDTH + LANE_WIDTH / 2;
        const int tx = x + seg.toLane   * LANE_WIDTH + LANE_WIDTH / 2;

        switch (seg.type) {
        case models::LaneSegmentType::PassThrough:
            painter->drawLine(fx, y, fx, y + h);
            break;

        case models::LaneSegmentType::End:
            // Top half — incoming line into the commit dot.
            // Always same-lane in our current model.
            painter->drawLine(fx, y, fx, my);
            break;

        case models::LaneSegmentType::Start:
            // Bottom half — outgoing line on the commit's own lane.
            painter->drawLine(fx, my, fx, y + h);
            break;

        case models::LaneSegmentType::MergeRight:
        case models::LaneSegmentType::MergeLeft:
        case models::LaneSegmentType::SplitRight:
        case models::LaneSegmentType::SplitLeft: {
            // Bottom-half S-curve from commit lane to a different
            // lane. Cubic bezier with control points that hold each
            // end vertical for h/3 then sweep across — gives a smooth
            // curve that doesn't kink, regardless of horizontal
            // distance.
            QPainterPath path;
            path.moveTo(fx, my);
            path.cubicTo(fx, my + h / 3,
                         tx, y + 2 * h / 3,
                         tx, y + h);
            painter->drawPath(path);
            break;
        }
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
    // Same proxy-walk dance as paint() — the row's natural width
    // depends on the source model's graph data, not whatever the
    // table view has attached.
    const QAbstractItemModel* m = index.model();
    QModelIndex idx = index;
    while (auto* proxy = qobject_cast<const QAbstractProxyModel*>(m)) {
        idx = proxy->mapToSource(idx);
        m = proxy->sourceModel();
    }
    auto* model = qobject_cast<const models::CommitLogModel*>(m);
    int w = 80;
    if (model && idx.isValid()) {
        const auto* g = model->graphAt(idx.row());
        if (g) w = std::max(w, (g->maxLane + 2) * LANE_WIDTH);
    }
    return QSize(w, option.fontMetrics.height() + 4);
}

QColor RevisionGraphDelegate::laneColor(int idx) const {
    return laneColors[static_cast<size_t>(idx) % laneColors.size()];
}

} // namespace gitbolt::editor
