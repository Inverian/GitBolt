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

private:
    QTableView* tableView_ = nullptr;
    models::CommitLogModel* model_ = nullptr;
};

} // namespace gitbolt::widgets
