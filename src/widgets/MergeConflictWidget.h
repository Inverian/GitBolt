#pragma once

#include "git/Merge.h"

#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>
#include <optional>
#include <vector>

class QEvent;
class QLabel;

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

    /// Resolved text for every conflict, index-aligned with the
    /// vector passed to setConflicts(). Valid once
    /// allConflictsResolved fires (onMarkAllResolved saves the
    /// current editor and fills unvisited conflicts with "ours"
    /// before emitting); the host zips these with the conflict
    /// paths to write files and stage them.
    std::vector<QString> allResolvedContents() const { return resolvedContents_; }

    /// For each conflict on a symbolic link, the side its result is:
    /// Ours, or Theirs once Accept Theirs was clicked last. The host
    /// stages that side as it is rather than the result's text, which
    /// the editor doesn't give back byte for byte (a no-break space in
    /// a link's target comes back as a space). None for the others.
    /// Index-aligned with allResolvedContents().
    std::vector<std::optional<gitbolt::git::ConflictSide>> allAcceptedSides() const;

    /// The conflicts currently loaded, as passed to setConflicts().
    const std::vector<gitbolt::git::MergeConflictEntry>& conflicts() const {
        return conflicts_;
    }

signals:
    void conflictResolved();
    void mergeAborted();
    void allConflictsResolved();

protected:
    /// A theme switch recolours the headings and the panes' tints.
    void changeEvent(QEvent* event) override;

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
    void applyColors();
    void showConflict(int index);
    void applyHighlighting(QPlainTextEdit* editor, const QColor& color);
    void updateNavigationState();

    // Three panels + result, and their headings
    QLabel* oursLabel_ = nullptr;
    QLabel* baseLabel_ = nullptr;
    QLabel* theirsLabel_ = nullptr;
    QLabel* resultLabel_ = nullptr;
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
    std::vector<gitbolt::git::ConflictSide> acceptedSides_;
};

} // namespace gitbolt::widgets
