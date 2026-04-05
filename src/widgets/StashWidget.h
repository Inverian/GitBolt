#pragma once

#include "git/Stash.h"

#include <QWidget>
#include <vector>

class QLabel;
class QMenu;
class QPushButton;
class QTableView;
class QToolBar;

namespace gitbolt::models {
class StashModel;
}

namespace gitbolt::widgets {

class DiffViewerWidget;

class StashWidget : public QWidget {
    Q_OBJECT
public:
    explicit StashWidget(QWidget* parent = nullptr);

    /// Populate with stash entries.
    void setStashes(std::vector<gitbolt::git::StashEntry> stashes);

    /// Clear stash list.
    void clear();

    /// Access to underlying model.
    models::StashModel* stashModel() const { return model_; }

signals:
    void stashSaveRequested(const QString& message, bool includeUntracked);
    void stashApplyRequested(int index);
    void stashPopRequested(int index);
    void stashDropRequested(int index);

private slots:
    void onSaveStash();
    void onApplyStash();
    void onPopStash();
    void onDropStash();
    void onViewChanges();
    void showContextMenu(const QPoint& pos);

private:
    void setupUi();
    int selectedStashIndex() const;

    QTableView* tableView_ = nullptr;
    models::StashModel* model_ = nullptr;
    QToolBar* toolbar_ = nullptr;

    QPushButton* saveBtn_ = nullptr;
    QPushButton* applyBtn_ = nullptr;
    QPushButton* popBtn_ = nullptr;
    QPushButton* dropBtn_ = nullptr;
};

} // namespace gitbolt::widgets
