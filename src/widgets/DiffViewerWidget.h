#pragma once

#include "git/Diff.h"

#include <QWidget>

class QAction;
class QLabel;
class QPlainTextEdit;
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

    explicit DiffViewerWidget(QWidget* parent = nullptr);

    /// Display a specific file from a full DiffResult by index.
    void setDiff(const gitbolt::git::DiffResult& diff, int fileIndex = 0);

    /// Display a single file's diff data directly.
    void setDiffForFile(const gitbolt::git::DiffFileEntry& file);

    /// Clear all displayed diff content.
    void clear();

    /// Current view mode.
    ViewMode viewMode() const { return mode_; }

public slots:
    void setViewMode(ViewMode mode);

private slots:
    void syncScrollLeft(int value);
    void syncScrollRight(int value);

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
};

} // namespace gitbolt::widgets
