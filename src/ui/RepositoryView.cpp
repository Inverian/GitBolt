#include "ui/RepositoryView.h"

#include <QFutureWatcher>
#include <QtConcurrent>

#include "conf/SettingsService.h"
#include "git/Diff.h"
#include "models/CommitLogModel.h"
#include "services/GitService.h"
#include "widgets/BlameWidget.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/DiffViewerWidget.h"
#include "widgets/FileTreeWidget.h"
#include "widgets/LoadingOverlayWidget.h"
#include "widgets/RevisionGraphWidget.h"
#include "widgets/TerminalWidget.h"

#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>
#include <set>
#include <unordered_map>

namespace gitbolt::ui {

namespace {
constexpr const char* kRepoSplitterHKey = "repoSplitterH/v1";
constexpr const char* kRepoSplitterVKey = "repoSplitterV/v1";
// Bumped to v2 when the diff splitter's stretch factors were
// retuned to match the File Tree tab (2:5 instead of 1:3) — wiping
// any saved v1 state so users get the new matching default on next
// launch. They can still drag it to taste; the v2 key persists.
constexpr const char* kDiffSplitterKey  = "diffSplitter/v2";

// Item-data roles for the diff tab's file tree. Path is the full
// file/dir path used for filter matching. FileIndex is the offset
// into DiffResult::files for leaf items so a click can jump the
// diff viewer to that file; -1 marks a directory row.
constexpr int kPathRole      = Qt::UserRole + 1;
constexpr int kIsDirRole     = Qt::UserRole + 2;
constexpr int kFileIndexRole = Qt::UserRole + 3;
} // namespace

RepositoryView::RepositoryView(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

// ---------------------------------------------------------------------------
// setupUi — assemble the three-region GitExtensions-style layout
// ---------------------------------------------------------------------------
void RepositoryView::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // --- Left pane: branch tree --------------------------------------
    branchTreeWidget_ = new widgets::BranchTreeWidget(this);

    // --- Right pane: graph on top, inspector tabs on bottom ----------
    graphWidget_ = new widgets::RevisionGraphWidget(this);
    connect(graphWidget_, &widgets::RevisionGraphWidget::commitSelected,
            this, &RepositoryView::onCommitSelected);

    // Tab order matches the GitExtensions browse window:
    //   Commit | Diff | File tree | GPG | Console
    // The Output tab from the reference is intentionally omitted
    // (we don't have a separate output channel from the console).
    // Several of these are still placeholders — see the per-tab
    // build helpers below for what's real and what's stubbed.
    inspectorTabs_ = new QTabWidget(this);
    inspectorTabs_->setDocumentMode(true);
    inspectorTabs_->addTab(buildCommitInfoTab(), tr("Commit"));
    inspectorTabs_->addTab(buildDiffTab(),       tr("Diff"));
    inspectorTabs_->addTab(buildFileTreeTab(),   tr("File Tree"));
    inspectorTabs_->addTab(buildGpgTab(),        tr("GPG"));
    inspectorTabs_->addTab(buildConsoleTab(),    tr("Console"));

    rightVSplitter_ = new QSplitter(Qt::Vertical, this);
    rightVSplitter_->setChildrenCollapsible(false);
    rightVSplitter_->addWidget(graphWidget_);
    rightVSplitter_->addWidget(inspectorTabs_);
    // Stretch factors are the Qt-documented default allocation
    // weights — they only apply to "extra space" beyond each
    // child's sizeHint, NOT to the initial layout. So we also call
    // setSizes() explicitly in applyBottomPanePercent() once the
    // outer height is known (deferred to the first showEvent).
    rightVSplitter_->setStretchFactor(0, 3);
    rightVSplitter_->setStretchFactor(1, 2);

    // --- Outer horizontal splitter -----------------------------------
    mainHSplitter_ = new QSplitter(Qt::Horizontal, this);
    mainHSplitter_->setChildrenCollapsible(false);
    mainHSplitter_->addWidget(branchTreeWidget_);
    mainHSplitter_->addWidget(rightVSplitter_);
    mainHSplitter_->setStretchFactor(0, 0);
    mainHSplitter_->setStretchFactor(1, 1);
    // Explicit first-launch sizes — 18% / 82% on the default
    // 1280 pt window. After the user drags the splitter the saved
    // state in QSettings takes precedence (see restoreSplitterState
    // in showEvent), so this only affects fresh installs.
    mainHSplitter_->setSizes(QList<int>{230, 1050});

    layout->addWidget(mainHSplitter_);
}

// Commit Info tab — three vertical sections:
//
//   ┌──────┬─────────────────────────────────┐
//   │ Av   │ Author / Date / Hash / Parent / │  ← top row
//   │ atar │ Stats   (metadata browser)      │     fixed height
//   └──────┴─────────────────────────────────┘
//   ─────────────────────────────────────────  ← full-width separator
//   ┌─────────────────────────────────────────┐
//   │ Summary (bold)                          │
//   │ Message body (monospace, wraps)         │  ← bottom section
//   │                                         │     full width, expanding
//   │ Contained in branches: …                │
//   └─────────────────────────────────────────┘
//
// Two QTextBrowsers are used because Qt has no clean way to make
// the message body in a single browser span past the avatar
// column. Splitting into two stacked browsers with a real
// QFrame::HLine between them gives the layout requested in the
// reference mockup.
QWidget* RepositoryView::buildCommitInfoTab()
{
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    // ----- Top: avatar + metadata --------------------------------
    // The top row's height is set DYNAMICALLY in showCommitDetails
    // based on the QTextDocument's natural laid-out height — that
    // way the metadata block is always exactly tall enough to fit
    // its content with no scrolling regardless of how many parent,
    // child, or committer rows the commit has. The avatar's
    // square size is updated in lockstep so it always exactly
    // fills its allocated square.
    commitInfoTopRow_ = new QWidget(page);
    auto* h = new QHBoxLayout(commitInfoTopRow_);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(12);

    avatarLabel_ = new QLabel(commitInfoTopRow_);
    // Initial size — overridden on first commit selection by
    // showCommitDetails to match the actual metadata height.
    avatarLabel_->setFixedSize(120, 120);
    avatarLabel_->setAlignment(Qt::AlignCenter);
    avatarLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // Transparent until a commit is selected.
    avatarLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background: transparent; color: transparent; }"));
    h->addWidget(avatarLabel_, 0, Qt::AlignTop);

    detailBrowser_ = new QTextBrowser(commitInfoTopRow_);
    detailBrowser_->setOpenExternalLinks(false);
    detailBrowser_->setFrameShape(QFrame::NoFrame);
    detailBrowser_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailBrowser_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Match the panel chrome instead of QTextBrowser's default
    // white. The HTML body's background is also set transparent
    // in the metadata HTML so the document doesn't paint over
    // this.
    detailBrowser_->setStyleSheet(QStringLiteral(
        "QTextBrowser { background: transparent; border: none; }"));
    h->addWidget(detailBrowser_, 1);

    // Initial height — overridden on first commit selection.
    commitInfoTopRow_->setFixedHeight(120);
    v->addWidget(commitInfoTopRow_);

    // ----- Full-width horizontal separator -----------------------
    auto* sep = new QFrame(page);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Plain);
    sep->setStyleSheet(QStringLiteral("QFrame { color: #ccc; }"));
    v->addWidget(sep);

    // ----- Bottom: full-width message body -----------------------
    messageBrowser_ = new QTextBrowser(page);
    messageBrowser_->setOpenExternalLinks(false);
    messageBrowser_->setPlaceholderText(tr("Select a commit to view details"));
    messageBrowser_->setFrameShape(QFrame::NoFrame);
    // Same transparency trick as the metadata browser — blend
    // into the panel chrome instead of painting a white plate.
    messageBrowser_->setStyleSheet(QStringLiteral(
        "QTextBrowser { background: transparent; border: none; }"));
    v->addWidget(messageBrowser_, 1);

    return page;
}

// File tree tab — interactive browser of the repo at the selected
// commit. Built around FileTreeWidget which handles its own
// internal layout (filter, tree, preview pane). RepositoryView
// just feeds it the active repo + selected commit.
QWidget* RepositoryView::buildFileTreeTab()
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);

    fileTreeWidget_ = new widgets::FileTreeWidget(page);
    l->addWidget(fileTreeWidget_);

    // Context-menu actions. All read gitService_ at click time
    // (it's injected via setGitService after construction).
    connect(fileTreeWidget_, &widgets::FileTreeWidget::openExternallyRequested,
            this, &RepositoryView::openFileExternally);
    connect(fileTreeWidget_, &widgets::FileTreeWidget::showHistoryRequested,
            this, &RepositoryView::showFileHistory);
    connect(fileTreeWidget_, &widgets::FileTreeWidget::blameRequested,
            this, &RepositoryView::showBlameForFile);

    return page;
}

// Open the working-tree copy of a repo-relative path with the OS
// default application. The file tree shows blobs at a specific
// commit, but for "open externally" the workdir version is what
// users expect (it's editable, and it exists as a real file —
// historical blobs would need a temp-file export).
void RepositoryView::openFileExternally(const QString& path)
{
    if (!gitService_ || !gitService_->isOpen())
        return;
    const QString workdir = QString::fromStdString(
        gitService_->withRepository(
            [](git::Repository& r) { return r.workdir(); }));
    const QString full = QDir(workdir).filePath(path);
    if (!QFileInfo::exists(full)) {
        QMessageBox::information(this, tr("Open Externally"),
            tr("%1 does not exist in the working tree (it may only "
               "exist at an older commit).").arg(path));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(full));
}

// Compact file-history popup: every commit that touched the path
// (rename-tracking via --follow), newest first. Double-clicking a
// row (or the Go to Commit button) jumps the revision graph to
// that commit. Built as an ad-hoc dialog matching how MainWindow
// does its one-off pickers; if file history grows features (diffs
// per revision, checkout-file-at) it should graduate to its own
// class in src/dialogs/.
void RepositoryView::showFileHistory(const QString& path)
{
    if (!gitService_ || !gitService_->isOpen())
        return;

    // %x09 = tab separator; fields: full sha, short sha, date,
    // author, subject.
    auto out = gitService_->process().run(
        {"log", "--follow", "--date=short",
         "--pretty=format:%H%x09%h%x09%ad%x09%an%x09%s", "--",
         path.toStdString()});
    if (!out.ok() || !out.value().success()) {
        QMessageBox::warning(this, tr("File History"),
            tr("Could not load history for %1").arg(path));
        return;
    }

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("History — %1").arg(path));
    dlg->setObjectName(QStringLiteral("dlg.fileHistory"));
    conf::SettingsService::applyConfiguredSize(dlg, "filehistory");

    auto* layout = new QVBoxLayout(dlg);

    auto* tree = new QTreeWidget(dlg);
    tree->setHeaderLabels(
        {tr("Commit"), tr("Date"), tr("Author"), tr("Message")});
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);

    const QString stdoutText =
        QString::fromStdString(out.value().stdoutData);
    const QStringList lines =
        stdoutText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList cols = line.split(QLatin1Char('\t'));
        if (cols.size() < 5)
            continue;
        auto* item = new QTreeWidgetItem(
            {cols[1], cols[2], cols[3], cols[4]});
        item->setData(0, Qt::UserRole, cols[0]); // full sha
        tree->addTopLevelItem(item);
    }
    tree->header()->resizeSection(0, 90);
    tree->header()->resizeSection(1, 90);
    tree->header()->resizeSection(2, 140);
    layout->addWidget(new QLabel(
        tr("%1 revision(s) touch this file:").arg(tree->topLevelItemCount()),
        dlg));
    layout->addWidget(tree, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    auto* gotoBtn = buttons->addButton(tr("Go to Commit"),
                                       QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);

    auto jumpToSelected = [this, tree, dlg]() {
        auto* item = tree->currentItem();
        if (!item)
            return;
        const QString sha = item->data(0, Qt::UserRole).toString();
        if (graphWidget_ && graphWidget_->selectCommit(sha)) {
            dlg->close();
        } else {
            // Paged log: the commit may simply not be loaded yet.
            QMessageBox::information(dlg, tr("File History"),
                tr("Commit %1 isn't in the loaded portion of the "
                   "log — scroll the revision list further back "
                   "and try again.").arg(sha.left(7)));
        }
    };
    connect(gotoBtn, &QPushButton::clicked, dlg, jumpToSelected);
    connect(tree, &QTreeWidget::itemDoubleClicked, dlg,
            [jumpToSelected](QTreeWidgetItem*, int) { jumpToSelected(); });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);

    dlg->show();
}

// Per-line blame in a modeless dialog hosting the (previously
// dormant) BlameWidget. The data flow mirrors the service's other
// async results: blameFile() computes on a worker under the repo
// mutex and emits blameReady; the dialog consumes only results for
// ITS path, so "Blame Before" re-blames (same path, earlier
// revision) update this dialog while any blame dialog on a
// different file is unaffected.
void RepositoryView::showBlameForFile(const QString& path)
{
    if (!gitService_ || !gitService_->isOpen())
        return;

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Blame — %1").arg(path));
    dlg->setObjectName(QStringLiteral("dlg.blame"));
    conf::SettingsService::applyConfiguredSize(dlg, "blame");

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* blame = new widgets::BlameWidget(dlg);
    layout->addWidget(blame);

    connect(gitService_, &services::GitService::blameReady,
            blame, [blame, path](git::BlameResult result) {
        if (QString::fromStdString(result.path) == path)
            blame->setBlameResult(std::move(result));
    });

    // Row click → jump the revision graph. Silently ignore commits
    // outside the loaded log window (clicking around an old file
    // would otherwise nag once per click).
    connect(blame, &widgets::BlameWidget::commitSelected,
            this, [this](const git::ObjectId& id) {
        if (graphWidget_)
            graphWidget_->selectCommit(
                QString::fromStdString(id.toHex()));
    });

    // "Blame Before" → re-blame at the selected commit's parent.
    // resolveRef handles the "<sha>^" revspec; the root commit has
    // no parent and surfaces as an operationFailed toast.
    connect(blame, &widgets::BlameWidget::blameBeforeRequested,
            this, [this](const git::ObjectId& commitId,
                         const QString& filePath) {
        gitService_->blameFile(filePath,
            QString::fromStdString(commitId.toHex())
                + QStringLiteral("^"));
    });

    gitService_->blameFile(path);
    dlg->show();
}

// GPG tab — placeholder. GitExtensions surfaces commit signature
// verification status here ("Commit is not signed" / "Good signature
// from <key>"). We'll wire this up when GPG verification is added
// to GitService; for now keep the chrome consistent.
QWidget* RepositoryView::buildGpgTab()
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);

    auto* placeholder = new QLabel(
        tr("GPG signature verification — coming soon"), page);
    placeholder->setAlignment(Qt::AlignCenter);
    QFont f = placeholder->font();
    f.setItalic(true);
    placeholder->setFont(f);
    placeholder->setStyleSheet(QStringLiteral("color: palette(mid);"));

    l->addWidget(placeholder);
    return page;
}

// Console tab — hosts an interactive PTY-backed terminal. The
// widget forks a real shell on first show with the repository
// path as its initial cwd, so users can run any command line
// tool (git, ls, grep, etc.) exactly like in iTerm/Terminal.app.
QWidget* RepositoryView::buildConsoleTab()
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);

    terminalWidget_ = new widgets::TerminalWidget(page);
    l->addWidget(terminalWidget_);
    return page;
}

// Diff tab — file list on the left, full diff viewer on the right.
//
// Above the file list sits a placeholder filter input that lets
// users narrow the changed-files tree by substring. The diff
// base used to be named in a small gray header above the filter,
// but that information already shows up in the commit row the
// user just clicked, so we drop the header to reclaim vertical
// space for the file tree.
QWidget* RepositoryView::buildDiffTab()
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);

    diffSplitter_ = new QSplitter(Qt::Horizontal, page);
    diffSplitter_->setChildrenCollapsible(false);

    // Left side: header + filter + file list, stacked vertically.
    auto* leftPane = new QWidget(page);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(4, 4, 2, 4);
    leftLayout->setSpacing(4);

    diffFilterInput_ = new QLineEdit(leftPane);
    diffFilterInput_->setPlaceholderText(tr("Filter files…"));
    diffFilterInput_->setClearButtonEnabled(true);
    diffFilterInput_->setToolTip(
        tr("Substring filter for the changed files tree"));
    // Live-filter the changed files tree. The proxy filters on the
    // full-path role with recursive filtering enabled, so typing
    // a directory name keeps that subtree visible and typing a
    // basename surfaces matching leaves with their ancestors.
    connect(diffFilterInput_, &QLineEdit::textChanged,
            this, [this](const QString& text) {
                if (!changedFilesProxy_ || !changedFilesTree_)
                    return;
                changedFilesProxy_->setFilterFixedString(text.trimmed());
                // Diffs are usually small enough that "everything
                // expanded" is the most useful default — both for
                // unfiltered scanning and for showing matches.
                changedFilesTree_->expandAll();
            });
    leftLayout->addWidget(diffFilterInput_);

    changedFilesTree_ = new QTreeView(leftPane);
    changedFilesTree_->setHeaderHidden(true);
    changedFilesTree_->setUniformRowHeights(true);
    changedFilesTree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    changedFilesTree_->setAnimated(false);
    changedFilesTree_->setIndentation(16);

    changedFilesModel_ = new QStandardItemModel(this);
    changedFilesProxy_ = new QSortFilterProxyModel(this);
    changedFilesProxy_->setSourceModel(changedFilesModel_);
    changedFilesProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    changedFilesProxy_->setRecursiveFilteringEnabled(true);
    // Filter on the full-path role rather than the display string —
    // leaf items show only their basename, so a substring like
    // "src/foo" only matches via the path role on directories and
    // their leaf descendants.
    changedFilesProxy_->setFilterRole(kPathRole);
    changedFilesTree_->setModel(changedFilesProxy_);

    leftLayout->addWidget(changedFilesTree_, 1);

    diffWidget_ = new widgets::DiffViewerWidget(page);

    diffSplitter_->addWidget(leftPane);
    diffSplitter_->addWidget(diffWidget_);
    // Same 2:5 ratio as FileTreeWidget's splitter so the Diff and
    // File Tree tabs give the user the same amount of horizontal
    // space for their left-pane file list.
    diffSplitter_->setStretchFactor(0, 2);
    diffSplitter_->setStretchFactor(1, 5);

    l->addWidget(diffSplitter_);
    return page;
}

// ---------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------
void RepositoryView::setGitService(services::GitService* service)
{
    gitService_ = service;
    // The file tree drives its own libgit2 reads (tree walk + blob
    // preview). It gets the service — not a raw Repository*, which
    // dies on every repo switch — so its reads run under the repo
    // lock via GitService::withRepository.
    if (fileTreeWidget_)
        fileTreeWidget_->setGitService(service);
}

void RepositoryView::setCommitLogModel(models::CommitLogModel* model)
{
    commitModel_ = model;
    graphWidget_->setModel(model);
}

void RepositoryView::setSettingsService(conf::SettingsService* settings)
{
    settings_ = settings;

    // Live-apply the bottom pane default whenever the user changes
    // the UI Design slider in the settings dialog. A bit coarse —
    // settingsChanged fires for any setting write, not just ours —
    // but applyBottomPanePercent is a cheap setSizes() call so
    // it's fine to re-run on every change.
    if (settings_) {
        connect(settings_, &conf::SettingsService::settingsChanged,
                this, &RepositoryView::applyBottomPanePercent);
    }
}

// ---------------------------------------------------------------------------
// Data forwarders
// ---------------------------------------------------------------------------
void RepositoryView::setBranches(std::vector<git::BranchInfo> branches)
{
    if (branchTreeWidget_)
        branchTreeWidget_->setBranches(std::move(branches));
}

void RepositoryView::setTags(std::vector<git::TagInfo> tags)
{
    if (branchTreeWidget_)
        branchTreeWidget_->setTags(std::move(tags));
}

void RepositoryView::setSubmodules(std::vector<git::SubmoduleInfo> submodules)
{
    if (branchTreeWidget_)
        branchTreeWidget_->setSubmodules(std::move(submodules));
}

void RepositoryView::setStashes(std::vector<git::StashEntry> stashes)
{
    if (branchTreeWidget_)
        branchTreeWidget_->setStashes(std::move(stashes));
}

widgets::RevisionGraphWidget* RepositoryView::revisionGraph() const
{
    return graphWidget_;
}

widgets::BranchTreeWidget* RepositoryView::branchTree() const
{
    return branchTreeWidget_;
}

widgets::TerminalWidget* RepositoryView::terminal() const
{
    return terminalWidget_;
}

// Forward the repository path to the embedded terminal. The
// TerminalWidget::changeDirectory call handles both states
// (stash cwd if not yet running, send `cd <path>` if already
// running) so we can route both "first open" and "switched repo"
// events through the same entry point.
void RepositoryView::setRepositoryPath(const QString& path)
{
    if (!terminalWidget_ || path.isEmpty())
        return;
    terminalWidget_->changeDirectory(path);
}

void RepositoryView::resetInspectorTabs()
{
    // Branch tree — clear filter so stale text doesn't hide the
    // new repo's branches.
    if (branchTreeWidget_)
        branchTreeWidget_->clear();

    // Commit tab — clear metadata and message
    if (avatarLabel_) {
        avatarLabel_->clear();
        avatarLabel_->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; color: transparent; }"));
    }
    if (detailBrowser_)
        detailBrowser_->clear();
    if (messageBrowser_) {
        messageBrowser_->clear();
        messageBrowser_->setPlaceholderText(tr("Select a commit to view details"));
    }

    // Diff tab — clear file tree and diff viewer
    if (diffFilterInput_)
        diffFilterInput_->clear();
    if (changedFilesModel_)
        changedFilesModel_->clear();
    if (diffWidget_)
        diffWidget_->clear();

    // File tree tab
    if (fileTreeWidget_)
        fileTreeWidget_->clear();

    // Switch back to the Commit tab
    if (inspectorTabs_)
        inspectorTabs_->setCurrentIndex(0);
}

// ---------------------------------------------------------------------------
// Loading overlay
// ---------------------------------------------------------------------------
void RepositoryView::showLoading(const QString& message)
{
    if (!loadingOverlay_)
        loadingOverlay_ = new widgets::LoadingOverlayWidget(this);
    loadingOverlay_->showOverlay(message);
}

void RepositoryView::hideLoading()
{
    if (loadingOverlay_)
        loadingOverlay_->hideOverlay();
}

// ---------------------------------------------------------------------------
// Splitter persistence
// ---------------------------------------------------------------------------
QByteArray RepositoryView::saveRepoSplitterH() const
{
    return mainHSplitter_ ? mainHSplitter_->saveState() : QByteArray{};
}

QByteArray RepositoryView::saveRepoSplitterV() const
{
    return rightVSplitter_ ? rightVSplitter_->saveState() : QByteArray{};
}

QByteArray RepositoryView::saveDiffSplitter() const
{
    return diffSplitter_ ? diffSplitter_->saveState() : QByteArray{};
}

// ---------------------------------------------------------------------------
// Bottom pane default — UI Design setting
// ---------------------------------------------------------------------------
//
// Stretch factors alone don't give you "40% of the window" at
// first layout — they only apportion EXTRA space beyond each
// child's sizeHint. To pin the bottom inspector pane at a given
// percent we have to call setSizes() explicitly once the
// splitter's outer height is known (post-show). This is called
// from the first showEvent (if no saved splitter state exists)
// and from SettingsService::settingsChanged when the user moves
// the UI Design slider — in both cases it snaps the splitter to
// the configured ratio, overriding any current drag-state.
void RepositoryView::applyBottomPanePercent()
{
    if (!rightVSplitter_ || !settings_)
        return;

    // If the splitter hasn't been laid out yet, height() is 0 and
    // setSizes({0,0}) would collapse everything. Just bail — the
    // showEvent below will call us again on first layout.
    const int total = rightVSplitter_->height();
    if (total <= 0)
        return;

    const int pct = qBound(10, settings_->bottomPanePercent(), 90);
    const int bottom = (total * pct) / 100;
    const int top    = total - bottom;
    rightVSplitter_->setSizes(QList<int>{top, bottom});
}

// QSplitter::restoreState is only safe after the splitter has
// children added AND has been laid out at least once. Deferring the
// restore until the first showEvent is the Qt-recommended pattern;
// the `restored_` bool makes sure we don't clobber user drags on
// later shows (e.g. the stacked widget swapping back in).
void RepositoryView::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);

    if (restored_ || !settings_)
        return;

    const QByteArray h = settings_->restoreSplitterState(
        QString::fromLatin1(kRepoSplitterHKey));
    if (!h.isEmpty() && mainHSplitter_)
        mainHSplitter_->restoreState(h);

    const QByteArray v = settings_->restoreSplitterState(
        QString::fromLatin1(kRepoSplitterVKey));
    if (!v.isEmpty() && rightVSplitter_) {
        rightVSplitter_->restoreState(v);
    } else {
        // No saved state — this is a fresh install (or the user
        // has just reset their layout). Apply the UI Design
        // percent as the initial split. Running it AFTER the
        // widget has been shown guarantees rightVSplitter_->
        // height() is non-zero, which setSizes() needs.
        applyBottomPanePercent();
    }

    const QByteArray d = settings_->restoreSplitterState(
        QString::fromLatin1(kDiffSplitterKey));
    if (!d.isEmpty() && diffSplitter_)
        diffSplitter_->restoreState(d);

    restored_ = true;
}

// ---------------------------------------------------------------------------
// Commit selection handling — unchanged from before, just points at
// the new parent widgets inside the inspector tabs.
// ---------------------------------------------------------------------------
void RepositoryView::onCommitSelected(const QString& commitHash)
{
    if (!commitModel_)
        return;

    // Empty hash means the user deselected the current commit
    // (modifier-click on the selected row in the revision graph).
    // Wipe the commit-specific inspector content so stale data
    // from the previous commit doesn't sit in the panels. We
    // explicitly DON'T touch the branch tree here — that's owned
    // by the repo, not by the current selection — only the commit
    // details, diff, and file-tree views get blanked.
    if (commitHash.isEmpty()) {
        if (avatarLabel_) {
            avatarLabel_->clear();
            avatarLabel_->setStyleSheet(QStringLiteral(
                "QLabel { background: transparent; color: transparent; }"));
        }
        if (detailBrowser_)
            detailBrowser_->clear();
        if (messageBrowser_) {
            messageBrowser_->clear();
            messageBrowser_->setPlaceholderText(
                tr("Select a commit to view details"));
        }
        if (diffFilterInput_)
            diffFilterInput_->clear();
        if (changedFilesModel_)
            changedFilesModel_->clear();
        if (diffWidget_)
            diffWidget_->clear();
        if (fileTreeWidget_)
            fileTreeWidget_->clear();
        return;
    }

    git::ObjectId targetId = git::ObjectId::fromHex(commitHash.toStdString());

    for (int row = 0; row < commitModel_->rowCount(); ++row) {
        const auto* commit = commitModel_->commitAt(row);
        if (commit && commit->id == targetId) {
            showCommitDetails(*commit);
            showCommitDiff(commit->id);
            // Load the file tree for this commit. The widget holds
            // the GitService (wired in setGitService) and resolves
            // the live repository per read, so no per-selection
            // pointer re-bind is needed anymore.
            if (fileTreeWidget_)
                fileTreeWidget_->setCommit(commit->id);
            return;
        }
    }
}

// Commit tab renderer — produces an HTML document that mirrors the
// GitExtensions Commit tab layout: an avatar block on the left
// (initials over a colored square) followed by structured field
// rows, then the commit message body, then a footer listing the
// branches and tags that contain this commit.
//
// "Contained in branches" is computed via `git branch --all
// --contains <sha>`. We shell out to git rather than walk libgit2
// branches manually because libgit2 has no direct equivalent —
// you'd have to walk every branch and check reachability, which
// is O(branches × commits). Git's own implementation is fast.
// The call is synchronous on the UI thread; on a typical repo it
// returns in <50ms, and we cap branch enumeration with the timeout
// in the GitProcess::run call below to avoid hangs.
void RepositoryView::showCommitDetails(const git::CommitData& commit)
{
    const QString summary = QString::fromStdString(commit.summary).toHtmlEscaped();
    const QString name    = QString::fromStdString(commit.author.name).toHtmlEscaped();
    const QString email   = QString::fromStdString(commit.author.email).toHtmlEscaped();
    const QString fullHex = QString::fromStdString(commit.id.toHex()).toHtmlEscaped();

    // Committer is shown as a separate row only if it differs from
    // the author — common after a rebase, cherry-pick, or merge,
    // but visual noise on the typical "I authored and committed it
    // myself" case.
    const bool committerDiffers =
        (commit.author.name  != commit.committer.name) ||
        (commit.author.email != commit.committer.email);
    const QString committerName  = QString::fromStdString(commit.committer.name).toHtmlEscaped();
    const QString committerEmail = QString::fromStdString(commit.committer.email).toHtmlEscaped();

    // Author initials for the avatar (first letter of first and
    // last whitespace-separated tokens; "?" if the name is empty).
    auto initialsFor = [](const QString& n) -> QString {
        const QStringList parts = n.split(QRegularExpression(QStringLiteral("\\s+")),
                                          Qt::SkipEmptyParts);
        if (parts.isEmpty())
            return QStringLiteral("?");
        QString first = parts.first().left(1).toUpper();
        if (parts.size() == 1)
            return first;
        return first + parts.last().left(1).toUpper();
    };
    const QString initials = initialsFor(QString::fromStdString(commit.author.name));

    // Deterministic color from a hash of the email so the same
    // author always gets the same avatar color across commits.
    auto colorFor = [](const QString& key) -> QString {
        if (key.isEmpty())
            return QStringLiteral("#888");
        const uint h = qHash(key);
        const int hue = static_cast<int>(h % 360u);
        return QString::fromLatin1("hsl(%1,55%,45%)").arg(hue);
    };
    const QString avatarBg = colorFor(QString::fromStdString(commit.author.email));

    // Avatar text is set up-front; the size + stylesheet are
    // applied AFTER the metadata HTML is laid out so we can
    // match the avatar height to the actual content height
    // (see the dynamic resize block at the bottom of this fn).
    if (avatarLabel_)
        avatarLabel_->setText(initials);

    // Author / commit time in a readable form.
    const auto authorTimeT = std::chrono::system_clock::to_time_t(commit.author.when);
    const QString authorDate = QDateTime::fromSecsSinceEpoch(
        static_cast<qint64>(authorTimeT)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    // Parent rows — always emitted. Root commits get a single
    // placeholder row instead of being silently omitted, so the
    // table layout is consistent across all commits.
    //
    // The hash <a> uses an INLINE style for font-family / font-size
    // rather than a <code> tag + CSS rule because QTextDocument's
    // default <code> rendering ignores CSS sizing and renders at
    // a noticeably larger point size than the surrounding body
    // text — verified visually against express.git's merge commits.
    // Inline styles always win in QTextDocument regardless of any
    // built-in element defaults.
    // No `text-decoration` here — leave it unset so QTextDocument's
    // default link underline applies, matching the mailto links in
    // the left column. The two link kinds should look identical.
    static const QString kHashLinkStyle = QStringLiteral(
        "font-family:Menlo,Consolas,monospace;font-size:11px;"
        "color:#1f6feb;");
    QString parentRows;
    if (commit.parentIds.empty()) {
        parentRows = QStringLiteral(
            "<tr><td class='lbl'>Parent:</td>"
            "<td class='val'><i style='color:#888;'>(none — root commit)</i></td></tr>");
    } else {
        for (const auto& pid : commit.parentIds) {
            const QString hex = QString::fromStdString(pid.toHex()).toHtmlEscaped();
            parentRows += QStringLiteral(
                "<tr><td class='lbl'>Parent:</td>"
                "<td class='val'><a href='gitbolt://commit/%1' style='%3'>%2</a></td></tr>")
                .arg(hex, hex, kHashLinkStyle);
        }
    }

    // Child rows — always emitted. The current model is the
    // (paged) revision list, so any commit in the model that has
    // the current commit in its parentIds is a child. Show all of
    // them — for fork points this produces multiple rows. When no
    // child is found (the commit is HEAD, or its child hasn't been
    // paged in yet) emit a placeholder so the row still appears
    // in the table.
    QString childRows;
    if (commitModel_) {
        for (int row = 0; row < commitModel_->rowCount(); ++row) {
            const auto* other = commitModel_->commitAt(row);
            if (!other)
                continue;
            for (const auto& parentOf : other->parentIds) {
                if (parentOf == commit.id) {
                    const QString hex = QString::fromStdString(other->id.toHex()).toHtmlEscaped();
                    childRows += QStringLiteral(
                        "<tr><td class='lbl'>Child:</td>"
                        "<td class='val'><a href='gitbolt://commit/%1' style='%3'>%2</a></td></tr>")
                        .arg(hex, hex, kHashLinkStyle);
                    break;
                }
            }
        }
    }
    if (childRows.isEmpty()) {
        childRows = QStringLiteral(
            "<tr><td class='lbl'>Child:</td>"
            "<td class='val'><i style='color:#888;'>(none — head of branch)</i></td></tr>");
    }

    // Committer row (only when different from author).
    QString committerRow;
    if (committerDiffers) {
        const auto committerTimeT = std::chrono::system_clock::to_time_t(commit.committer.when);
        const QString committerDate = QDateTime::fromSecsSinceEpoch(
            static_cast<qint64>(committerTimeT)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        committerRow = QStringLiteral(
            "<tr><td class='lbl'>Committer:</td>"
            "<td class='val'>%1 &lt;<a href='mailto:%2'>%2</a>&gt; &nbsp; <span style='color:#888;'>%3</span></td></tr>")
            .arg(committerName, committerEmail, committerDate);
    }

    // Diff stats — files-changed count + total +N/-M lines. Pulled
    // straight from libgit2's diff result so we don't have to
    // re-walk anything. The lookup is fast (libgit2 caches the
    // tree pair) but only run when we actually have a repository.
    QString statsRow;
    if (gitService_ && gitService_->isOpen()) {
        auto diffResult = gitService_->withRepository(
            [&](git::Repository& r) { return r.diffCommit(commit.id); });
        if (diffResult) {
            const auto& d = *diffResult;
            statsRow = QStringLiteral(
                "<tr><td class='lbl'>Stats:</td>"
                "<td class='val'>%1 file%2 changed, "
                "<span style='color:#22863a;'>+%3</span> "
                "<span style='color:#d73a49;'>−%4</span></td></tr>")
                .arg(d.files.size())
                .arg(d.files.size() == 1 ? QString{} : QStringLiteral("s"))
                .arg(d.totalAdditions)
                .arg(d.totalDeletions);
        }
    }

    // Commit message body (everything after the summary line).
    QString bodyHtml;
    if (commit.message.size() > commit.summary.size()) {
        QString body = QString::fromStdString(commit.message).mid(
            static_cast<int>(commit.summary.size())).trimmed();
        if (!body.isEmpty()) {
            bodyHtml = QStringLiteral(
                "<div class='msgbody'><pre>%1</pre></div>")
                .arg(body.toHtmlEscaped());
        }
    }

    // ----- "Contained in branches" -------------------------------
    // Shell out to `git branch --all --contains <sha>` so we get
    // both local and remote-tracking branches that have this commit
    // in their history. `--format=%(refname:short)` gives us clean
    // branch names with no leading "* " marker for the current
    // branch. Sha is a 40-char hex (or 64 for SHA-256 repos), so
    // there's no shell-escaping concern even though we're passing
    // it as a process arg directly.
    //
    // Failure modes we handle:
    //   - no repo open       → "(no repo open)"
    //   - git CLI missing /
    //     timeout / non-zero → "(unable to query)"
    //   - empty list (orphan
    //     commit, or only on
    //     a detached HEAD)   → "(none)"
    //   - too many to fit    → first 10, then "+N more"
    //
    // We cap output at 10 names because commits early in history
    // can be reachable from hundreds of branches in active repos,
    // which would blow out the inspector's vertical space.
    // The query itself now runs on a pool thread (see below, after
    // the HTML is assembled) — it shells out to git and can take
    // seconds on big repos, and running it synchronously here made
    // every arrow-key scrub through the log stall per row. The
    // browser shows a placeholder until the worker reports back; a
    // token drops stale replies once the selection has moved on.
    QString containsBranchesText = (gitService_ && gitService_->isOpen())
        ? QStringLiteral("<span class='muted'>(querying…)</span>")
        : QStringLiteral("<span class='muted'>(no repo open)</span>");

    // ----- Top browser: 2-column metadata grid ------------------
    // No summary/body/footer here — those live in messageBrowser_
    // below the full-width separator. The avatar lives outside
    // both browsers in its own QLabel.
    //
    // Two-column layout (matches GitExtensions):
    //   LEFT  — people fields:  Author / Date / Committer / Stats
    //   RIGHT — hash fields:    Commit hash / Parent(s) / Child(ren)
    //
    // The two columns are independent sub-tables inside a 2-cell
    // outer table so they top-align and can have unequal row counts
    // (e.g. an octopus merge with many parents on the right).
    //
    // Sizing matches the GitExtensions Commit tab proportions:
    // 11 px body font, 1 px top/bottom cell padding, 4 px label
    // padding-right. Per-row height ends up around 16 px.
    const QString leftRows = QStringLiteral(
        "<tr><td class='lbl'>Author:</td>"
        "<td class='val'>%1 &lt;<a href='mailto:%2'>%2</a>&gt;</td></tr>"
        "<tr><td class='lbl'>Date:</td>"
        "<td class='val'>%3</td></tr>"
        "%4"   // committerRow (optional)
        "%5"   // statsRow (optional)
    ).arg(name, email, authorDate, committerRow, statsRow);

    // Commit hash is the only hash row that ISN'T a link, so we
    // can put the inline mono style directly on the <td> instead
    // of an <a>. Same font-family / font-size as the linked
    // hashes above so the right column is visually uniform.
    const QString rightRows = QStringLiteral(
        "<tr><td class='lbl'>Commit hash:</td>"
        "<td class='val' style='font-family:Menlo,Consolas,monospace;font-size:11px;'>%1</td></tr>"
        "%2"   // parentRows (always; multiple on merges)
        "%3"   // childRows  (always; multiple at fork points)
    ).arg(fullHex, parentRows, childRows);

    // Tallest column drives the dynamic top-row height below.
    const int leftRowCount  = static_cast<int>(leftRows.count(QStringLiteral("<tr>")));
    const int rightRowCount = static_cast<int>(rightRows.count(QStringLiteral("<tr>")));
    const int maxRows       = std::max(leftRowCount, rightRowCount);

    const QString metadataHtml = QStringLiteral(R"HTML(
<style>
  body {
    font-family: -apple-system, sans-serif;
    font-size: 11px; margin: 0;
    background: transparent;
  }
  table.meta { border-spacing: 0; }
  table.meta td { padding: 1px 4px; vertical-align: top; font-size: 11px; }
  td.lbl { color: #666; white-space: nowrap; padding-right: 4px; }
  td.val { font-size: 11px; }
  td.val a { color: #1f6feb; text-decoration: none; }
</style>

<table style="border-spacing:0;">
  <tr>
    <td style="vertical-align:top; padding:0;">
      <table class="meta">%1</table>
    </td>
    <td style="vertical-align:top; padding:0 0 0 28px;">
      <table class="meta">%2</table>
    </td>
  </tr>
</table>
)HTML")
        .arg(leftRows, rightRows);

    detailBrowser_->setHtml(metadataHtml);

    // ----- Bottom browser: summary, message body, contained-in --
    const QString messageHtml = QStringLiteral(R"HTML(
<style>
  body {
    font-family: -apple-system, sans-serif;
    font-size: 12px; margin: 0;
    background: transparent;
  }
  div.summary { font-weight: bold; padding: 4px 0 0 0; margin-bottom: 10px; font-size: 13px; }
  div.msgbody pre {
    white-space: pre-wrap; margin: 0; padding: 6px 0 2px 0;
    font-family: Menlo, Consolas, monospace; font-size: 11px;
  }
  hr.sep { border: none; border-top: 1px solid #ddd; margin: 12px 0; }
  div.contains { color: #444; padding: 2px 0; }
  div.contains a { color: #1f6feb; text-decoration: none; }
  span.muted { color: #888; font-style: italic; }
</style>

<div class="summary">%1</div>
%2

<hr class="sep" />

<div class="contains">Contained in branches: %3</div>
<div class="contains">Contained in no tag</div>
<div class="contains">Derives from no tag</div>
)HTML")
        .arg(summary, bodyHtml, containsBranchesText);

    messageBrowser_->setHtml(messageHtml);

    // Kick the contained-in query on a worker and patch the browser
    // when it lands. The GitProcess is snapshotted HERE (on the GUI
    // thread, under the service's lock) and used freely on the pool
    // thread — QProcess execution is independent of libgit2 state.
    if (gitService_ && gitService_->isOpen()) {
        const quint64 token = ++containsQueryToken_;
        const std::string sha = commit.id.toHex();
        git::GitProcess proc = gitService_->process();
        // Rebuild the surrounding HTML on completion: everything but
        // the contains row is captured by value.
        const QString htmlTemplate = messageHtml;
        const QString placeholder = containsBranchesText;

        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this,
                [this, watcher, token, htmlTemplate, placeholder]() {
            watcher->deleteLater();
            if (token != containsQueryToken_ || !messageBrowser_)
                return;  // selection moved on — stale reply
            QString patched = htmlTemplate;
            patched.replace(placeholder, watcher->result());
            messageBrowser_->setHtml(patched);
        });
        watcher->setFuture(QtConcurrent::run([proc, sha]() -> QString {
            auto out = proc.run(
                {"branch", "--all", "--contains", sha,
                 "--format=%(refname:short)"},
                /*timeoutMs=*/5000);
            if (!out || !out->success())
                return QStringLiteral(
                    "<span class='muted'>(unable to query)</span>");

            QStringList branches;
            const QString stdoutText =
                QString::fromStdString(out->stdoutData);
            const auto lines = stdoutText.split(QChar('\n'),
                                                Qt::SkipEmptyParts);
            for (const auto& line : lines) {
                const QString trimmed = line.trimmed();
                if (trimmed.isEmpty())
                    continue;
                // Skip "<remote>/HEAD" symbolic refs — they're
                // aliases for whatever branch HEAD points at on
                // the remote (typically already in our list as
                // "<remote>/main"), so listing them is noise.
                if (trimmed.endsWith(QLatin1String("/HEAD")))
                    continue;
                branches << trimmed.toHtmlEscaped();
            }
            if (branches.isEmpty())
                return QStringLiteral(
                    "<span class='muted'>(none)</span>");
            constexpr int kMaxShown = 10;
            QString text =
                branches.mid(0, kMaxShown).join(QStringLiteral(", "));
            if (branches.size() > kMaxShown) {
                text += QStringLiteral(
                    " <span class='muted'>+%1 more</span>")
                    .arg(branches.size() - kMaxShown);
            }
            return text;
        }));
    }

    // ----- Dynamic resize: top row + avatar fit content exactly -
    // Sizing matches the GitExtensions Commit tab. Each row at
    // our 11px body font is ~18 px tall in practice (13 px line
    // height + 1 px top/bottom cell padding + ~3 px from QTextDoc
    // line spacing). The 16 px estimate from the original pass was
    // too tight — verified against a real commit with 4 rows of
    // metadata where the bottom row (Stats) was being clipped. The
    // +16 fudge accounts for the QTextBrowser document and viewport
    // margins. The driver is the TALLER of the two columns (people
    // vs hashes) — not the total <tr> count — since the two columns
    // top-align inside the outer 2-cell table. Clamped to:
    //   -  80 floor: even sparse root commits get a usable avatar
    //   - 180 ceiling: an octopus merge with many parents/children
    //     can't dominate the inspector
    if (commitInfoTopRow_) {
        const int needed = maxRows * 18 + 16;
        const int finalH = std::max(80, std::min(needed, 180));
        commitInfoTopRow_->setFixedHeight(finalH);
        if (avatarLabel_) {
            avatarLabel_->setFixedSize(finalH, finalH);
            // Font size scales with the avatar — ~45% of the
            // square's edge gives readable initials at every
            // height in the clamped range.
            const int fontPx = std::max(16, finalH * 9 / 20);
            avatarLabel_->setStyleSheet(QStringLiteral(
                "QLabel {"
                "  background: %1;"
                "  color: white;"
                "  font-size: %2px;"
                "  font-weight: bold;"
                "}").arg(avatarBg).arg(fontPx));
        }
    }
}

void RepositoryView::showCommitDiff(const git::ObjectId& commitId)
{
    // Drop any previous click handler — each call to showCommitDiff
    // captures a fresh DiffResult shared_ptr in its lambda, and we
    // don't want stale captures firing after the model is cleared.
    if (changedFilesTree_ && changedFilesTree_->selectionModel()) {
        disconnect(changedFilesTree_->selectionModel(),
                   &QItemSelectionModel::currentChanged,
                   this, nullptr);
    }

    if (changedFilesModel_)
        changedFilesModel_->clear();
    diffWidget_->clear();

    if (!gitService_ || !gitService_->isOpen())
        return;

    auto diffResult = gitService_->withRepository(
        [&](git::Repository& r) { return r.diffCommit(commitId); });
    if (!diffResult)
        return;

    auto diff = std::make_shared<git::DiffResult>(std::move(*diffResult));

    // Build the changed-files tree. Two passes so directory rows
    // come before file rows at every level: pass 1 collects each
    // unique ancestor path into a sorted std::set and creates a
    // QStandardItem for each, pass 2 appends the file leaves under
    // their parents.
    std::set<std::string> dirPaths;
    for (const auto& file : diff->files) {
        const std::string& p = file.path();
        auto slash = p.rfind('/');
        while (slash != std::string::npos) {
            std::string anc = p.substr(0, slash);
            dirPaths.insert(anc);
            slash = anc.rfind('/');
        }
    }

    QFileIconProvider iconProvider;
    const QIcon folderIcon = iconProvider.icon(QFileIconProvider::Folder);
    const QIcon fileIcon   = iconProvider.icon(QFileIconProvider::File);

    // path → item. Empty key represents the model root.
    std::unordered_map<std::string, QStandardItem*> dirByPath;
    dirByPath.reserve(dirPaths.size() + 1);
    dirByPath[std::string{}] = changedFilesModel_->invisibleRootItem();

    for (const std::string& dirPath : dirPaths) {
        std::string parentPath;
        const auto slash = dirPath.rfind('/');
        if (slash != std::string::npos)
            parentPath = dirPath.substr(0, slash);
        const std::string name = (slash == std::string::npos)
                                     ? dirPath : dirPath.substr(slash + 1);

        auto* dirItem = new QStandardItem(folderIcon,
                                          QString::fromStdString(name));
        QFont f = dirItem->font();
        f.setBold(true);
        dirItem->setFont(f);
        dirItem->setEditable(false);
        dirItem->setData(QString::fromStdString(dirPath), kPathRole);
        dirItem->setData(true, kIsDirRole);
        dirItem->setData(-1, kFileIndexRole);

        QStandardItem* parent = dirByPath[parentPath];
        parent->appendRow(dirItem);
        dirByPath[dirPath] = dirItem;
    }

    for (size_t i = 0; i < diff->files.size(); ++i) {
        const auto& file = diff->files[i];

        QString prefix;
        switch (file.status) {
            case git::DiffStatus::Added:    prefix = QStringLiteral("[A] "); break;
            case git::DiffStatus::Deleted:  prefix = QStringLiteral("[D] "); break;
            case git::DiffStatus::Modified: prefix = QStringLiteral("[M] "); break;
            case git::DiffStatus::Renamed:  prefix = QStringLiteral("[R] "); break;
            case git::DiffStatus::Copied:   prefix = QStringLiteral("[C] "); break;
            default:                        prefix = QStringLiteral("[?] "); break;
        }

        // Per-file +N/-M stats. DiffFileEntry doesn't carry these
        // pre-tallied so we walk the hunk lines. Binary files
        // report (binary) instead.
        int adds = 0;
        int dels = 0;
        for (const auto& hunk : file.hunks) {
            for (const auto& line : hunk.lines) {
                if (line.type == git::DiffLineType::Addition) ++adds;
                else if (line.type == git::DiffLineType::Deletion) ++dels;
            }
        }

        const std::string& fullPath = file.path();
        std::string parentPath;
        const auto slash = fullPath.rfind('/');
        if (slash != std::string::npos)
            parentPath = fullPath.substr(0, slash);
        const std::string basename = (slash == std::string::npos)
                                         ? fullPath
                                         : fullPath.substr(slash + 1);

        QString label = prefix + QString::fromStdString(basename);
        if (file.isBinary)
            label += QStringLiteral("   (binary)");
        else if (adds || dels)
            label += QStringLiteral("   +%1 −%2").arg(adds).arg(dels);

        auto* item = new QStandardItem(fileIcon, label);
        item->setEditable(false);
        item->setData(QString::fromStdString(fullPath), kPathRole);
        item->setData(false, kIsDirRole);
        item->setData(static_cast<int>(i), kFileIndexRole);
        if (file.isBinary)
            item->setForeground(QColor(0x88, 0x88, 0x88));

        QStandardItem* parent = dirByPath[parentPath];
        parent->appendRow(item);
    }

    changedFilesTree_->expandAll();

    if (!diff->files.empty()) {
        diffWidget_->setDiff(*diff, 0);
    }

    // Selection model handles both clicks and arrow-key navigation,
    // so users can scrub through the file list with the keyboard
    // the same way QListWidget supported it.
    connect(changedFilesTree_->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this, diff](const QModelIndex& proxyIndex,
                         const QModelIndex& /*previous*/) {
                if (!proxyIndex.isValid())
                    return;
                const QModelIndex sourceIndex =
                    changedFilesProxy_->mapToSource(proxyIndex);
                QStandardItem* item =
                    changedFilesModel_->itemFromIndex(sourceIndex);
                if (!item)
                    return;
                const int fileIndex = item->data(kFileIndexRole).toInt();
                if (fileIndex >= 0
                    && fileIndex < static_cast<int>(diff->files.size())) {
                    diffWidget_->setDiff(*diff, fileIndex);
                }
            });
}

} // namespace gitbolt::ui
