#pragma once

#include "git/ObjectId.h"

#include <QWidget>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolBar;

namespace gitbolt::widgets {

class CherryPickWidget : public QWidget {
    Q_OBJECT
public:
    explicit CherryPickWidget(QWidget* parent = nullptr);

    /// Retrieve the list of commits selected for cherry-pick.
    std::vector<git::ObjectId> selectedCommits() const;

    /// Whether "no commit" mode is checked.
    bool noCommitMode() const;

    /// Set status text (progress / result feedback).
    void setStatusText(const QString& text);

    /// Clear all content.
    void clear();

signals:
    void cherryPickRequested(const std::vector<gitbolt::git::ObjectId>& commits);
    void cherryPickAborted();

private slots:
    void onAddCommit();
    void onRemoveSelected();
    void onCherryPick();
    void onAbort();

private:
    void setupUi();
    bool isValidHex(const QString& hex) const;

    QLineEdit*   commitInput_   = nullptr;
    QPushButton* addBtn_        = nullptr;
    QListWidget* commitList_    = nullptr;
    QPushButton* removeBtn_     = nullptr;
    QCheckBox*   noCommitCheck_ = nullptr;
    QPushButton* pickBtn_       = nullptr;
    QPushButton* abortBtn_      = nullptr;
    QLabel*      statusLabel_   = nullptr;
    QToolBar*    toolbar_       = nullptr;
};

} // namespace gitbolt::widgets
