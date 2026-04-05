#pragma once

#include "git/Repository.h"
#include "models/TagModel.h"

#include <QAction>
#include <QTableView>
#include <QToolBar>
#include <QWidget>

namespace gitbolt::widgets {

class TagWidget : public QWidget {
    Q_OBJECT
public:
    explicit TagWidget(QWidget* parent = nullptr);

    void setTags(std::vector<gitbolt::git::TagInfo> tags);
    void clear();

    models::TagModel* tagModel() const { return model_; }

signals:
    void createTagRequested();
    void deleteTagRequested(const QString& name);
    void pushTagRequested(const QString& name);
    void checkoutTagRequested(const QString& name);

private slots:
    void onContextMenu(const QPoint& pos);
    void onDeleteClicked();
    void onPushClicked();

private:
    void setupUI();
    QString selectedTagName() const;

    QTableView* tableView_ = nullptr;
    models::TagModel* model_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QAction* createAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* pushAction_ = nullptr;
};

} // namespace gitbolt::widgets
