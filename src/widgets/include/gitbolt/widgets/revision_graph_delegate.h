#pragma once
#include "gitbolt/models/commit_log_model.h"
#include <QStyledItemDelegate>
#include <QColor>
#include <array>

namespace gitbolt::widgets {

class RevisionGraphDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr int LANE_WIDTH = 16;
    static constexpr int NODE_RADIUS = 4;

    explicit RevisionGraphDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    static const std::array<QColor, 16> laneColors;
    QColor laneColor(int idx) const;
};

} // namespace gitbolt::widgets
