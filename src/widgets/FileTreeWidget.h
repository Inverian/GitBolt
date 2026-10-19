#pragma once

#include "git/ObjectId.h"
#include "git/Tree.h"

#include <QWidget>

#include <atomic>
#include <memory>
#include <vector>

class QEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QTimer;
class QTreeView;
class QModelIndex;

namespace gitbolt::git { class Repository; }
namespace gitbolt::services { class GitService; }

namespace gitbolt::widgets {

/// File tree browser for the inspector — left pane is a hierarchical
/// QTreeView of every file at the selected commit, right pane is a
/// read-only preview of whatever blob the user clicks. Mirrors the
/// GitExtensions File tree tab and lifts the most useful pieces from
/// `git gui browser`, tig's tree view, and Fork's commit browser:
///
///   - Click a file → preview its contents (binary detection
///     suppresses the dump for non-text blobs).
///   - Filter input narrows the tree by case-insensitive name match.
///   - Right-click on any entry → "Copy Path" / "Show History"
///     (history is a stub for now; the entry exists so the menu
///     mirrors the GitExtensions one).
///   - Sizes shown alongside file names; directories are bold.
///
/// The tree walk (and the sort of what it finds) runs on the service's
/// read thread (GitService::readAsync): on the GUI thread, a commit
/// with a huge tree froze the window for the walk. That thread has the
/// GUI thread's 8 MB stack, so the walk (libgit2's git_tree_walk
/// recurses once per level) takes a tree as deep as it did; what bounds
/// the depth is a limit in libgit2, not the thread. A blob preview
/// reads one blob, at most kMaxPreviewBytes of it, synchronously.
class FileTreeWidget : public QWidget {
    Q_OBJECT
public:
    explicit FileTreeWidget(QWidget* parent = nullptr);

    /// Set the git service whose open repository this widget browses.
    /// Calling with nullptr clears the view. Safe to call repeatedly
    /// when switching repos. The widget never holds a raw
    /// git::Repository* — that pointer dies on every repo switch,
    /// and tree walks must run under the service's repo lock anyway
    /// (GitService::withRepository).
    void setGitService(services::GitService* service);

    /// Load the tree at the given commit. If the commit is the
    /// same as the currently-loaded one this is a no-op. Pass an
    /// empty/zero ObjectId to clear the view.
    void setCommit(const git::ObjectId& commitId);

    void clear();

signals:
    /// Emitted when the user chooses "Show History…" in the
    /// context menu. RepositoryView opens the file-history popup.
    /// Path is repo-relative.
    void showHistoryRequested(const QString& path);

    /// Emitted when the user picks "Open Externally" — the host
    /// resolves the repo workdir + path and shells out to the
    /// platform's default opener.
    void openExternallyRequested(const QString& path);

    /// Emitted when the user picks "Blame" — the host opens the
    /// blame view for this repo-relative path.
    void blameRequested(const QString& path);

protected:
    /// A theme switch recolours the preview's heading.
    void changeEvent(QEvent* event) override;

private slots:
    void onTreeClicked(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);
    void onFilterTextChanged(const QString& text);

private:
    void setupUi();
    void applyColors();
    void rebuildTree();
    /// Empties the tree and says it's loading.
    void showLoading();
    /// The walk of currentCommit_ is in, sorted: fills the tree.
    void showTree(const std::vector<git::TreeEntry>& entries);
    void showBlobPreview(const git::ObjectId& blobId, const QString& path,
                         quint64 size);
    static bool looksBinary(const QByteArray& data);
    static QString formatSize(quint64 bytes);

    services::GitService* svc_     = nullptr;
    git::ObjectId    currentCommit_;
    // Bumped by every walk asked for and by clear(), and shared with the
    // worker: a walk's reply for an earlier commit is dropped, and a walk
    // superseded stops where it is, or doesn't run if its turn hadn't
    // come.
    std::shared_ptr<std::atomic<quint64>> walkToken_ =
        std::make_shared<std::atomic<quint64>>(0);
    // Until a walk is in, the tree on show (the previous commit's) stays,
    // so a walk in within milliseconds, as most are, shows in one change.
    // Emptied at once, the tree painted empty, under "Loading file tree…",
    // on every selection, and flickered as the user arrowed through the
    // log. That lasts kLoadingDelayMs at most, counted from the first
    // walk asked for that is still out, so arrowing on through slow ones
    // can't keep a commit's tree up for another: then it is emptied and
    // says it's loading.
    QTimer* loadingTimer_ = nullptr;

    QSplitter*             splitter_     = nullptr;
    QLineEdit*             filterInput_  = nullptr;
    QTreeView*             treeView_     = nullptr;
    QStandardItemModel*    model_        = nullptr;
    QSortFilterProxyModel* proxy_        = nullptr;

    QLabel*                previewHeader_ = nullptr;
    QPlainTextEdit*        preview_      = nullptr;
};

} // namespace gitbolt::widgets
