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
#include <atomic>
#include <memory>
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
class QTimer;
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

    // The selected commit's diff is read on a worker (GitService::
    // readAsync): Repository::diffCommit walks the whole tree of a root
    // commit, and here on the GUI thread a commit with a huge or deeply
    // nested tree froze the window, for minutes. It fills the Stats row
    // and the Diff tab when it lands. Monotonic like the token above,
    // and shared with the worker: a reply for an earlier selection is
    // dropped, and a read the selection has left behind stops where it
    // is, or doesn't run at all if its turn hadn't come.
    std::shared_ptr<std::atomic<quint64>> diffQueryToken_ =
        std::make_shared<std::atomic<quint64>>(0);

    // A commit's Stats row, once its diff is in (ok false: it couldn't
    // be read, and the row says why, `error`, in place of the numbers).
    struct DiffStats {
        git::ObjectId commit;
        bool ok = false;
        QString error;
        size_t files = 0;
        size_t additions = 0;
        size_t deletions = 0;
    };
    // The Stats of the latest diff in, to render the commit again with
    // (a theme switch) without asking for it again.
    std::optional<DiffStats> shownStats_;
    // Until the selected commit's diff is in, the Diff tab keeps the
    // previous commit's files and the Stats row is blank, so a diff in
    // within milliseconds, as most are, shows in one change. Emptied at
    // once, the tab and the row painted an empty or "(counting…)" state
    // on every selection, and flickered as the user arrowed through the
    // log. (The row doesn't keep the previous commit's numbers: beside
    // this one's details they would read as its own.) That lasts
    // kDiffLoadingDelayMs at most, counted from the first selection
    // whose diff is still out, so arrowing on through slow ones can't
    // keep one commit's files up for another: then the tab is emptied
    // and the row says "(counting…)" (showDiffLoading), and diffLoading_
    // stays set, for the commits selected meanwhile too, until a diff
    // comes in.
    QTimer* diffLoadingTimer_ = nullptr;
    bool diffLoading_ = false;

    // The avatar's colour for the commit on show (showCommitDetails),
    // from the author's email as shown: util::kMaxLineChars of it at
    // most. fitCommitInfoTopRow, which runs on every re-wrap, hashed
    // the whole email, and an email of tens of MB (git takes one) cost
    // a decode and a hash of all of it each time the window resized.
    QString avatarColor_;

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
    /// The diff of `commitId` is in (null: it couldn't be read, `error`
    /// says why): its Stats row.
    void showDiffStats(const git::ObjectId& commitId, const git::DiffResult* diff,
                       const QString& error = {});
    void showDiffFiles(std::shared_ptr<git::DiffResult> diff);
    void clearDiffFiles();
    /// The selected commit's diff isn't in after kDiffLoadingDelayMs.
    void showDiffLoading();
    /// Writes the Commit tab's Stats value over in place, in the colours
    /// of the theme in effect: `stats`'s numbers, or why its diff
    /// couldn't be read, or "(counting…)" without.
    void setStatsRow(const DiffStats* stats);

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
    QLabel*                       diffFailure_       = nullptr;
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
