#pragma once

#include "git/ObjectId.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
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
/// All loads are synchronous and bounded by the size of the tree
/// at the selected commit — for a libgit2-cached tree this is in
/// the few-millisecond range even for the linux kernel. We don't
/// background-thread the walk yet; that's a follow-up if a user
/// hits the latency on a giant repo.
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

private slots:
    void onTreeClicked(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);
    void onFilterTextChanged(const QString& text);

private:
    void setupUi();
    void rebuildTree();
    void showBlobPreview(const git::ObjectId& blobId, const QString& path,
                         quint64 size);
    static bool looksBinary(const QByteArray& data);
    static QString formatSize(quint64 bytes);

    services::GitService* svc_     = nullptr;
    git::ObjectId    currentCommit_;

    QSplitter*             splitter_     = nullptr;
    QLineEdit*             filterInput_  = nullptr;
    QTreeView*             treeView_     = nullptr;
    QStandardItemModel*    model_        = nullptr;
    QSortFilterProxyModel* proxy_        = nullptr;

    QLabel*                previewHeader_ = nullptr;
    QPlainTextEdit*        preview_      = nullptr;
};

} // namespace gitbolt::widgets
