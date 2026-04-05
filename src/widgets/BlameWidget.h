#pragma once

#include "git/ObjectId.h"
#include "models/BlameModel.h"

#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QToolBar>
#include <QWidget>

namespace gitbolt::widgets {

class BlameWidget : public QWidget {
    Q_OBJECT
public:
    explicit BlameWidget(QWidget* parent = nullptr);

    void setBlameResult(gitbolt::git::BlameResult result);
    void clear();

    models::BlameModel* blameModel() const { return model_; }

signals:
    void commitSelected(const gitbolt::git::ObjectId& id);
    void blameBeforeRequested(const gitbolt::git::ObjectId& commitId,
                              const QString& filePath);

private slots:
    void onRowClicked(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);
    void onBlameBeforeClicked();

private:
    void setupUI();

    QTableView* tableView_ = nullptr;
    models::BlameModel* model_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QLabel* filePathLabel_ = nullptr;
    QPushButton* blameBeforeBtn_ = nullptr;

    // Currently selected commit for "blame before" navigation
    gitbolt::git::ObjectId selectedCommitId_;
};

} // namespace gitbolt::widgets
