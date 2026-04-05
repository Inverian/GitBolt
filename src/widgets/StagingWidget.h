#pragma once

#include "git/Status.h"

#include <QWidget>
#include <vector>

class QListView;
class QPushButton;
class QLabel;
class QMenu;

namespace gitbolt::models {
class FileStatusModel;
}

namespace gitbolt::widgets {

class StagingWidget : public QWidget {
    Q_OBJECT

public:
    explicit StagingWidget(QWidget* parent = nullptr);

    /// Replace the full status entry list. The widget filters internally.
    void setEntries(std::vector<gitbolt::git::StatusEntry> entries);

    /// Clear both panels.
    void clear();

signals:
    // Staging operations
    void stageRequested(const QString& path);
    void unstageRequested(const QString& path);
    void stageAllRequested();
    void unstageAllRequested();
    void discardRequested(const QString& path);

    // Inform other widgets that a file was selected for diffing
    void fileSelected(const QString& path);

    // Request to open a file in the code editor
    void openInEditorRequested(const QString& path);

private slots:
    void onUnstagedDoubleClicked(const QModelIndex& index);
    void onStagedDoubleClicked(const QModelIndex& index);
    void onUnstagedSelectionChanged();
    void onStagedSelectionChanged();
    void showUnstagedContextMenu(const QPoint& pos);
    void showStagedContextMenu(const QPoint& pos);

private:
    void setupUi();

    models::FileStatusModel* unstagedModel_ = nullptr;
    models::FileStatusModel* stagedModel_   = nullptr;

    QListView*   unstagedView_    = nullptr;
    QListView*   stagedView_      = nullptr;
    QLabel*      unstagedLabel_   = nullptr;
    QLabel*      stagedLabel_     = nullptr;
    QPushButton* stageAllBtn_     = nullptr;
    QPushButton* unstageAllBtn_   = nullptr;
};

} // namespace gitbolt::widgets
