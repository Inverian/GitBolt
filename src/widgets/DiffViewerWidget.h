#pragma once

#include "git/Diff.h"

#include <QWidget>

#include <utility>
#include <vector>

class QAction;
class QLabel;
class QPlainTextEdit;
class QPoint;
class QScrollBar;
class QSplitter;
class QToolBar;

namespace gitbolt::editor {
class DiffSyntaxHighlighter;
}

namespace gitbolt::widgets {

class DiffViewerWidget : public QWidget {
    Q_OBJECT

public:
    enum ViewMode {
        Unified,
        SideBySide
    };

    /// Whether (and with which verb) the unified view offers
    /// hunk/line staging in its context menu. Hosts that show a
    /// WORKDIR→INDEX diff set Stage; hosts that show an
    /// INDEX→HEAD diff set Unstage; read-only hosts (history
    /// diffs) leave it at None. The widget only emits requests —
    /// the host builds the patch and applies it.
    enum class HunkAction {
        None,
        Stage,
        Unstage,
    };

    explicit DiffViewerWidget(QWidget* parent = nullptr);

    /// Display a specific file from a full DiffResult by index.
    void setDiff(const gitbolt::git::DiffResult& diff, int fileIndex = 0);

    /// Display a single file's diff data directly.
    void setDiffForFile(const gitbolt::git::DiffFileEntry& file);

    /// Clear all displayed diff content (also resets the hunk
    /// action mode to None — hosts re-arm it when they show the
    /// next stageable diff).
    void clear();

    /// Current view mode.
    ViewMode viewMode() const { return mode_; }

    void setHunkActionMode(HunkAction mode) { hunkAction_ = mode; }
    HunkAction hunkActionMode() const { return hunkAction_; }

    /// The file entry currently displayed — hosts use this to
    /// build hunk/line patches for the staging requests below.
    bool hasFile() const { return hasFile_; }
    const gitbolt::git::DiffFileEntry& currentFile() const {
        return currentFile_;
    }

signals:
    /// Context-menu staging requests from the unified view. The
    /// verb (stage vs unstage) is implied by the HunkAction mode
    /// the host configured. `lineIndices` index into
    /// currentFile().hunks[hunkIndex].lines.
    void hunkActionRequested(int hunkIndex);
    void linesActionRequested(int hunkIndex, QList<int> lineIndices);

public slots:
    void setViewMode(ViewMode mode);

private slots:
    void syncScrollLeft(int value);
    void syncScrollRight(int value);
    void onUnifiedContextMenu(const QPoint& pos);

private:
    void setupUi();
    void renderUnified(const gitbolt::git::DiffFileEntry& file);
    void renderSideBySide(const gitbolt::git::DiffFileEntry& file);
    void applyViewMode();

    ViewMode mode_ = Unified;

    // Toolbar
    QToolBar* toolbar_      = nullptr;
    QAction*  unifiedAct_   = nullptr;
    QAction*  sideBySideAct_ = nullptr;
    QLabel*   fileLabel_    = nullptr;

    // Unified mode
    QPlainTextEdit* unifiedEditor_ = nullptr;
    editor::DiffSyntaxHighlighter* unifiedHighlighter_ = nullptr;

    // Side-by-side mode
    QSplitter*      sideSplitter_ = nullptr;
    QPlainTextEdit* leftEditor_   = nullptr;
    QPlainTextEdit* rightEditor_  = nullptr;

    // Cached for re-render on mode switch
    gitbolt::git::DiffFileEntry currentFile_;
    bool hasFile_ = false;

    // Guard to prevent recursive scroll sync
    bool syncingScroll_ = false;

    // Hunk/line staging state. blockHunkLine_ maps each block
    // (visual line) of the unified editor to (hunk index, line
    // index within that hunk); (-1,-1) for the file header and
    // (h,-1) for hunk header rows. Rebuilt by renderUnified.
    HunkAction hunkAction_ = HunkAction::None;
    std::vector<std::pair<int, int>> blockHunkLine_;
};

} // namespace gitbolt::widgets
