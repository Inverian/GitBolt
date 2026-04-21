#pragma once
#include "models/CommitLogModel.h"
#include <QWidget>

class QTableView;

namespace gitbolt::widgets {

class RevisionGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit RevisionGraphWidget(QWidget* parent = nullptr);
    void setModel(models::CommitLogModel* model);
    models::CommitLogModel* model() const;

signals:
    void commitSelected(const QString& commitHash);

private slots:
    void onSelectionChanged();
    void resizeGraphColumn();
    /// Size Author/Date/Hash by content then add a padding margin so text
    /// isn't cramped against the next column. ResizeToContents alone packs
    /// columns tight with zero breathing room; we switch those columns to
    /// Interactive and recompute their widths ourselves whenever the row
    /// set changes.
    void resizeMetaColumns();

private:
    QTableView* tableView_ = nullptr;
    models::CommitLogModel* model_ = nullptr;
};

} // namespace gitbolt::widgets
