#pragma once

#include "git/Merge.h"

#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>
#include <vector>

namespace gitbolt::widgets {

class MergeConflictWidget : public QWidget {
    Q_OBJECT
public:
    explicit MergeConflictWidget(QWidget* parent = nullptr);

    void setConflicts(const std::vector<gitbolt::git::MergeConflictEntry>& conflicts);
    void clear();

    int currentConflictIndex() const { return currentConflict_; }
    int conflictCount() const { return static_cast<int>(conflicts_.size()); }
    QString resolvedContent() const;

signals:
    void conflictResolved();
    void mergeAborted();
    void allConflictsResolved();

private slots:
    void onAcceptOurs();
    void onAcceptTheirs();
    void onAcceptBoth();
    void onMarkAllResolved();
    void onAbortMerge();
    void onPrevConflict();
    void onNextConflict();

private:
    void setupUI();
    void showConflict(int index);
    void applyHighlighting(QPlainTextEdit* editor, const QColor& color);
    void updateNavigationState();

    // Three panels + result
    QPlainTextEdit* oursEditor_ = nullptr;
    QPlainTextEdit* baseEditor_ = nullptr;
    QPlainTextEdit* theirsEditor_ = nullptr;
    QPlainTextEdit* resultEditor_ = nullptr;

    // Buttons
    QPushButton* acceptOursBtn_ = nullptr;
    QPushButton* acceptTheirsBtn_ = nullptr;
    QPushButton* acceptBothBtn_ = nullptr;
    QPushButton* markAllResolvedBtn_ = nullptr;
    QPushButton* abortMergeBtn_ = nullptr;
    QPushButton* prevBtn_ = nullptr;
    QPushButton* nextBtn_ = nullptr;

    // Conflict tracking
    std::vector<gitbolt::git::MergeConflictEntry> conflicts_;
    int currentConflict_ = -1;
    std::vector<QString> resolvedContents_;
};

} // namespace gitbolt::widgets
