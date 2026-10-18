#pragma once
#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Diff.h"
#include "git/Stash.h"
#include "git/Submodule.h"
#include "git/Tag.h"
#include <QSet>
#include <QString>
#include <QWidget>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QSplitter;
class QStandardItemModel;
class QTabWidget;
class QTextBrowser;
class QTreeView;
class QShowEvent;
class QUrl;

namespace gitbolt::models   { class CommitLogModel; }
namespace gitbolt::services { class GitService;     }
namespace gitbolt::conf     { class SettingsService;}

namespace gitbolt::widgets {
class BranchTreeWidget;
class DiffViewerWidget;
class FileTreeWidget;
class LoadingOverlayWidget;
class RepoOperationBar;
class RevisionGraphWidget;
class TerminalWidget;
} // namespace gitbolt::widgets

namespace gitbolt::ui {

/// The repository browse view — three regions styled after the
/// GitExtensions browse window:
///
///   ┌────────────┬──────────────────────────────────────┐
///   │            │  Revision grid (commit log + graph)  │
///   │ BranchTree ├──────────────────────────────────────┤
///   │            │  Inspector tabs                      │
///   │            │   [Commit Info] [File Tree] [Diff]   │
///   └────────────┴──────────────────────────────────────┘
///
/// All staging/commit-message UI lives in a separate `CommitDialog`
/// opened from the MainWindow toolbar — this view is browse-only.
class RepositoryView : public QWidget {
    Q_OBJECT
public:
    explicit RepositoryView(QWidget* parent = nullptr);

    // Services
    void setGitService(services::GitService* service);
    void setCommitLogModel(models::CommitLogModel* model);
    void setSettingsService(conf::SettingsService* settings);

    // Data push-ins from MainWindow (forward to BranchTreeWidget)
    void setBranches(std::vector<git::BranchInfo> branches);
    void setTags(std::vector<git::TagInfo> tags);
    void setSubmodules(std::vector<git::SubmoduleInfo> submodules);
    void setStashes(std::vector<git::StashEntry> stashes);

    // Accessors
    widgets::RevisionGraphWidget* revisionGraph() const;
    widgets::BranchTreeWidget*    branchTree()    const;
    widgets::TerminalWidget*      terminal()      const;
    /// Across the top while a rebase / merge / cherry-pick / revert
    /// is in progress. MainWindow feeds and answers it.
    widgets::RepoOperationBar*    operationBar()  const;

    /// Tell the embedded terminal which directory to start in
    /// (or `cd` to if it's already running), and a bare repository's
    /// GIT_DIR (TerminalWidget::changeRepository). Called by MainWindow
    /// whenever a repository is opened.
    void setRepositoryPath(const QString& path, const QString& bareGitDir = {});

    /// Clear all inspector-tab content and reset to the Commit tab.
    /// Called by MainWindow when switching repositories so stale
    /// data from the previous repo isn't visible.
    void resetInspectorTabs();

    /// Busy overlay covering the whole repo view, shown while an
    /// asynchronous repository open is populating the models (the
    /// overlay also blocks interaction with the still-empty panes).
    /// MainWindow shows it when an open starts and hides it when
    /// the first commit-log page arrives (or the open fails).
    void showLoading(const QString& message);
    void hideLoading();

    // Splitter persistence — called by MainWindow at shutdown.
    QByteArray saveRepoSplitterH() const;
    QByteArray saveRepoSplitterV() const;
    QByteArray saveDiffSplitter()  const;

    /// Re-apply the bottom-pane percent from SettingsService to
    /// the inner vertical splitter. Called from the Settings dialog
    /// when the user changes the default, and from the first show
    /// when no saved splitter state exists. If the splitter's outer
    /// height is still zero (pre-layout), the call is a no-op —
    /// the percent is re-applied on the next showEvent.
    void applyBottomPanePercent();

public slots:
    void onCommitSelected(const QString& commitHash);

private slots:
    /// File Tree tab context-menu actions ("Open Externally" /
    /// "Show History…" / "Blame"). Paths are repo-relative.
    void openFileExternally(const QString& path);
    void showFileHistory(const QString& path);
    void showBlameForFile(const QString& path);

    /// A link clicked in the Commit tab: a Parent / Child hash selects
    /// that commit, an email opens the mail app; nothing else is
    /// followed.
    void openCommitLink(const QUrl& url);

protected:
    void showEvent(QShowEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    // Monotonic token for the async "contained in branches" query —
    // a reply older than the latest request is dropped instead of
    // overwriting the newer commit's details.
    quint64 containsQueryToken_ = 0;

    // The files whose history showFileHistory is loading.
    QSet<QString> historyLoading_;

    // The commit the Commit tab shows, rendered again in the colours of
    // a theme switched to (changeEvent).
    std::optional<git::CommitData> shownCommit_;
    bool renderingDetails_ = false;

    /// showFileHistory's dialog, for `git log`'s output on `path`.
    void showFileHistoryDialog(const QString& path, const QString& stdoutText);

    void setupUi();
    QWidget* buildCommitInfoTab();
    QWidget* buildDiffTab();
    QWidget* buildFileTreeTab();
    QWidget* buildGpgTab();
    QWidget* buildConsoleTab();
    void showCommitDetails(const git::CommitData& commit);
    /// Sizes the Commit tab's top row and avatar to the metadata as
    /// laid out; again whenever its laid-out size changes.
    void fitCommitInfoTopRow();
    void showCommitDiff(const git::ObjectId& commitId);

    // Services
    services::GitService*    gitService_  = nullptr;
    models::CommitLogModel*  commitModel_ = nullptr;
    conf::SettingsService*   settings_    = nullptr;

    // Left pane
    widgets::BranchTreeWidget*    branchTreeWidget_ = nullptr;

    // Right pane — top
    widgets::RevisionGraphWidget* graphWidget_      = nullptr;

    // Right pane — bottom (inspector tabs)
    QTabWidget*                   inspectorTabs_    = nullptr;

    // Commit Info tab
    QWidget*                      commitInfoTopRow_ = nullptr;
    QLabel*                       avatarLabel_      = nullptr;
    QTextBrowser*                 detailBrowser_    = nullptr;
    QTextBrowser*                 messageBrowser_   = nullptr;

    // Diff tab
    QLineEdit*                    diffFilterInput_   = nullptr;
    QTreeView*                    changedFilesTree_  = nullptr;
    QStandardItemModel*           changedFilesModel_ = nullptr;
    QSortFilterProxyModel*        changedFilesProxy_ = nullptr;
    widgets::DiffViewerWidget*    diffWidget_        = nullptr;
    QSplitter*                    diffSplitter_      = nullptr;

    // File tree tab — interactive browser of the repo at the
    // selected commit, with filter + preview pane.
    widgets::FileTreeWidget*      fileTreeWidget_   = nullptr;

    // Console tab — interactive PTY-backed terminal. The widget
    // forks a real shell with the repository path as its initial
    // cwd, so users can run any git command directly.
    widgets::TerminalWidget*      terminalWidget_   = nullptr;
    widgets::RepoOperationBar*    operationBar_     = nullptr;

    // Splitters
    QSplitter* mainHSplitter_ = nullptr;
    QSplitter* rightVSplitter_ = nullptr;

    // Busy overlay for async repository opens. Lazily created on
    // first showLoading() so the common fast-open path never pays
    // for it.
    widgets::LoadingOverlayWidget* loadingOverlay_ = nullptr;

    // One-shot restore guard (see showEvent).
    bool restored_ = false;
};

} // namespace gitbolt::ui
