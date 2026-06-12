#include "dialogs/CommitDialog.h"

#include "conf/SettingsService.h"
#include "git/Config.h"
#include "git/Diff.h"
#include "git/PatchBuilder.h"
#include "git/Repository.h"
#include "models/FileStatusModel.h"
#include "services/GitService.h"
#include "widgets/CommitMessageEdit.h"
#include "widgets/DiffViewerWidget.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QToolBar>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

namespace {
constexpr const char* kGeometryKey      = "commitDialogGeom/v2";
constexpr const char* kMainSplitterKey  = "commitMainSplitter/v2";
constexpr const char* kLeftSplitterKey  = "commitLeftSplitter/v2";
constexpr const char* kRightSplitterKey = "commitRightSplitter/v2";
} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CommitDialog::CommitDialog(services::GitService* svc,
                           conf::SettingsService* settings,
                           QWidget* parent)
    : QDialog(parent, Qt::Window | Qt::WindowCloseButtonHint |
                       Qt::WindowMinMaxButtonsHint)
    , svc_(svc)
    , settings_(settings)
{
    setModal(false);
    // Global dialog default — applied as the initial size. The
    // showEvent below may overwrite this with restoreGeometry
    // if the user has previously dragged the dialog AND the
    // "Restore previous dialog size" toggle is on (the default).
    // See Settings → UI Design → Default Dialog Size.
    //
    // CommitDialog uses its own kGeometryKey (commitDialogGeom/v2)
    // rather than the generic applyConfiguredSize() helper because
    // it also persists three internal splitter states alongside
    // the geometry — keeping all four reads in one showEvent path
    // is simpler than splitting them across two mechanisms.
    resize(conf::SettingsService::loadDefaultDialogSize());

    setupUi();
    wireConnections();
    readCommitterFromConfig();
    updateTitle();
    updateStatusBar();
}

CommitDialog::~CommitDialog() = default;

// ---------------------------------------------------------------------------
// UI assembly
// ---------------------------------------------------------------------------

void CommitDialog::setupUi()
{
    // ---- Models + filtering proxies ---------------------------------------
    // Two FileStatusModels share the same source data; each filters
    // internally by the staged/unstaged predicate. A QSortFilterProxyModel
    // on top of each gives the per-pane substring filter input.
    unstagedModel_ = new models::FileStatusModel(this);
    unstagedModel_->setStagedFilter(false);

    stagedModel_ = new models::FileStatusModel(this);
    stagedModel_->setStagedFilter(true);

    unstagedProxy_ = new QSortFilterProxyModel(this);
    unstagedProxy_->setSourceModel(unstagedModel_);
    unstagedProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    unstagedProxy_->setFilterKeyColumn(models::FileStatusModel::Path);

    stagedProxy_ = new QSortFilterProxyModel(this);
    stagedProxy_->setSourceModel(stagedModel_);
    stagedProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    stagedProxy_->setFilterKeyColumn(models::FileStatusModel::Path);

    // -----------------------------------------------------------------------
    // Left pane: vertical split between unstaged (top) and staged (bottom).
    // Each section is built from the same template:
    //
    //     ┌──────────────────────────────┐
    //     │ Section title                │
    //     │ [filter input]               │
    //     │ [QListView]                  │
    //     │ [primary button row]         │
    //     └──────────────────────────────┘
    //
    // I considered building a small SectionWidget helper class for this
    // but the file is short enough that two parallel inline sections
    // read more clearly than one parameterized factory.
    // -----------------------------------------------------------------------

    auto buildSectionTitleFont = [this]() {
        QFont f = font();
        f.setBold(true);
        return f;
    };
    const QFont sectionTitleFont = buildSectionTitleFont();

    // Tooltip explaining the status-code letters that appear in
    // both file lists. Built once and reused on both info icons —
    // the codes are universal (mirroring `git status --short`),
    // so duplicating the explanation per section would just be
    // noise. Rich HTML so QToolTip renders the table cleanly.
    const QString statusLegendTip = tr(
        "<b>File status codes</b>"
        "<table cellspacing='4'>"
        "<tr><td><b>M</b></td><td>Modified — content changed</td></tr>"
        "<tr><td><b>A</b></td><td>Added — new file, staged</td></tr>"
        "<tr><td><b>D</b></td><td>Deleted — removed from the working tree</td></tr>"
        "<tr><td><b>R</b></td><td>Renamed — moved to a new path</td></tr>"
        "<tr><td><b>U</b></td><td>Unmerged — has merge conflicts</td></tr>"
        "<tr><td><b>?</b></td><td>Untracked — new file, not yet staged</td></tr>"
        "</table>");

    // Builds a section header layout: bold title on the left,
    // spacer, and a small (i) icon on the right whose tooltip
    // shows the legend above. Used identically for both the
    // unstaged and staged sections.
    auto makeSectionHeader = [&](QLabel* title) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        row->addWidget(title);
        row->addStretch(1);
        // ⓘ (U+24D8) is the standard "info" glyph and renders
        // consistently across macOS / Windows / Linux without
        // needing a new SVG asset.
        auto* info = new QLabel(QStringLiteral("ⓘ"),
                                title->parentWidget());
        info->setToolTip(statusLegendTip);
        info->setCursor(Qt::WhatsThisCursor);
        // Muted gray so it doesn't compete with the bold title;
        // slightly larger so the glyph is easy to hit and read.
        info->setStyleSheet(QStringLiteral(
            "QLabel { color: palette(mid); font-size: 15px; "
            "padding: 0 2px; }"));
        row->addWidget(info);
        return row;
    };

    // ---- Unstaged section -------------------------------------------------
    auto* unstagedPanel = new QWidget(this);
    auto* unstagedLayout = new QVBoxLayout(unstagedPanel);
    unstagedLayout->setContentsMargins(6, 6, 6, 6);
    unstagedLayout->setSpacing(4);

    unstagedLabel_ = new QLabel(tr("Unstaged Changes"), unstagedPanel);
    unstagedLabel_->setFont(sectionTitleFont);
    unstagedLayout->addLayout(makeSectionHeader(unstagedLabel_));

    unstagedFilter_ = new QLineEdit(unstagedPanel);
    unstagedFilter_->setObjectName(QStringLiteral("commit.unstagedFilter"));
    unstagedFilter_->setPlaceholderText(tr("Filter unstaged files…"));
    unstagedFilter_->setClearButtonEnabled(true);
    unstagedLayout->addWidget(unstagedFilter_);

    unstagedView_ = new QListView(unstagedPanel);
    unstagedView_->setObjectName(QStringLiteral("commit.unstagedList"));
    unstagedView_->setModel(unstagedProxy_);
    unstagedView_->setModelColumn(models::FileStatusModel::Path);
    unstagedView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    unstagedView_->setAlternatingRowColors(true);
    unstagedLayout->addWidget(unstagedView_, 1);

    auto* unstagedBtnRow = new QHBoxLayout;
    unstagedBtnRow->setContentsMargins(0, 0, 0, 0);
    stageBtn_     = new QPushButton(tr("Stage"),    unstagedPanel);
    stageAllBtn_  = new QPushButton(tr("Stage All"), unstagedPanel);
    discardBtn_   = new QPushButton(tr("Discard"),   unstagedPanel);
    stageBtn_->setEnabled(false);
    discardBtn_->setEnabled(false);
    unstagedBtnRow->addWidget(stageBtn_);
    unstagedBtnRow->addWidget(stageAllBtn_);
    unstagedBtnRow->addWidget(discardBtn_);
    unstagedBtnRow->addStretch(1);
    unstagedLayout->addLayout(unstagedBtnRow);

    // ---- Staged section ---------------------------------------------------
    auto* stagedPanel = new QWidget(this);
    auto* stagedLayout = new QVBoxLayout(stagedPanel);
    stagedLayout->setContentsMargins(6, 6, 6, 6);
    stagedLayout->setSpacing(4);

    stagedLabel_ = new QLabel(tr("Staged Changes"), stagedPanel);
    stagedLabel_->setFont(sectionTitleFont);
    stagedLayout->addLayout(makeSectionHeader(stagedLabel_));

    stagedFilter_ = new QLineEdit(stagedPanel);
    stagedFilter_->setObjectName(QStringLiteral("commit.stagedFilter"));
    stagedFilter_->setPlaceholderText(tr("Filter staged files…"));
    stagedFilter_->setClearButtonEnabled(true);
    stagedLayout->addWidget(stagedFilter_);

    stagedView_ = new QListView(stagedPanel);
    stagedView_->setObjectName(QStringLiteral("commit.stagedList"));
    stagedView_->setModel(stagedProxy_);
    stagedView_->setModelColumn(models::FileStatusModel::Path);
    stagedView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    stagedView_->setAlternatingRowColors(true);
    stagedLayout->addWidget(stagedView_, 1);

    auto* stagedBtnRow = new QHBoxLayout;
    stagedBtnRow->setContentsMargins(0, 0, 0, 0);
    unstageBtn_    = new QPushButton(tr("Unstage"),    stagedPanel);
    unstageAllBtn_ = new QPushButton(tr("Unstage All"), stagedPanel);
    unstageBtn_->setEnabled(false);
    stagedBtnRow->addWidget(unstageBtn_);
    stagedBtnRow->addWidget(unstageAllBtn_);
    stagedBtnRow->addStretch(1);
    stagedLayout->addLayout(stagedBtnRow);

    // ---- Left splitter ----------------------------------------------------
    leftSplitter_ = new QSplitter(Qt::Vertical, this);
    leftSplitter_->setChildrenCollapsible(false);
    leftSplitter_->addWidget(unstagedPanel);
    leftSplitter_->addWidget(stagedPanel);
    leftSplitter_->setStretchFactor(0, 1);
    leftSplitter_->setStretchFactor(1, 1);

    // -----------------------------------------------------------------------
    // Right pane: vertical split between diff viewer (top) and commit
    // controls (bottom).
    // -----------------------------------------------------------------------

    diffView_ = new widgets::DiffViewerWidget(this);

    // ---- Commit controls panel -------------------------------------------
    // A two-column layout: button column on the left (Commit / Commit & Push
    // / Amend), message editor on the right. Mirrors the right side of the
    // GitExtensions commit window where the action buttons stack vertically
    // beside the multi-line message field.
    auto* commitPanel = new QWidget(this);
    auto* commitOuter = new QHBoxLayout(commitPanel);
    commitOuter->setContentsMargins(8, 8, 8, 8);
    commitOuter->setSpacing(8);

    auto* buttonCol = new QVBoxLayout;
    buttonCol->setContentsMargins(0, 0, 0, 0);
    buttonCol->setSpacing(4);

    commitBtn_     = new QPushButton(tr("Commit"),        commitPanel);
    commitPushBtn_ = new QPushButton(tr("Commit && Push"), commitPanel);
    amendCheck_    = new QCheckBox(tr("Amend last commit"), commitPanel);
    commitBtn_->setDefault(true);
    // Equal-width buttons stacked vertically — set a min width so the
    // amend checkbox doesn't squeeze the column narrower than the buttons.
    commitBtn_->setMinimumWidth(140);
    commitPushBtn_->setMinimumWidth(140);

    buttonCol->addWidget(commitBtn_);
    buttonCol->addWidget(commitPushBtn_);
    buttonCol->addWidget(amendCheck_);
    buttonCol->addStretch(1);

    commitOuter->addLayout(buttonCol);

    messageEdit_ = new widgets::CommitMessageEdit(commitPanel);
    messageEdit_->setObjectName(QStringLiteral("commit.message"));
    messageEdit_->setMinimumHeight(80);
    commitOuter->addWidget(messageEdit_, 1);

    // ---- Right splitter ---------------------------------------------------
    rightSplitter_ = new QSplitter(Qt::Vertical, this);
    rightSplitter_->setChildrenCollapsible(false);
    rightSplitter_->addWidget(diffView_);
    rightSplitter_->addWidget(commitPanel);
    rightSplitter_->setStretchFactor(0, 4);
    rightSplitter_->setStretchFactor(1, 1);

    // ---- Main horizontal splitter ----------------------------------------
    mainSplitter_ = new QSplitter(Qt::Horizontal, this);
    mainSplitter_->setChildrenCollapsible(false);
    mainSplitter_->addWidget(leftSplitter_);
    mainSplitter_->addWidget(rightSplitter_);
    mainSplitter_->setStretchFactor(0, 2);
    mainSplitter_->setStretchFactor(1, 5);

    // -----------------------------------------------------------------------
    // Inline error label — hidden until a commit fails. Sits above the
    // footer status strip so the user's typed message stays visible.
    // -----------------------------------------------------------------------
    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background:#5a1f1f; color:#ffdada; padding:6px 10px; }"));
    errorLabel_->hide();

    // -----------------------------------------------------------------------
    // Footer status strip (committer | branch | staged count). A simple
    // QFrame with three labels — much lighter weight than dragging in
    // QStatusBar (which carries its own size grip and message slot).
    // -----------------------------------------------------------------------
    auto* statusStrip = new QFrame(this);
    statusStrip->setFrameShape(QFrame::StyledPanel);
    statusStrip->setObjectName(QStringLiteral("CommitStatusStrip"));
    statusStrip->setStyleSheet(QStringLiteral(
        "#CommitStatusStrip { border-top: 1px solid palette(mid); "
        "background: palette(window); }"));

    auto* statusLayout = new QHBoxLayout(statusStrip);
    statusLayout->setContentsMargins(8, 4, 8, 4);
    statusLayout->setSpacing(16);

    committerLabel_ = new QLabel(statusStrip);
    committerLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    branchStatusLabel_ = new QLabel(statusStrip);
    stagedCountLabel_  = new QLabel(statusStrip);

    statusLayout->addWidget(committerLabel_, 1);
    statusLayout->addWidget(branchStatusLabel_, 0, Qt::AlignCenter);
    statusLayout->addWidget(stagedCountLabel_, 0, Qt::AlignRight);

    // -----------------------------------------------------------------------
    // Root layout
    // -----------------------------------------------------------------------
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(mainSplitter_, 1);
    root->addWidget(errorLabel_);
    root->addWidget(statusStrip);
}

// ---------------------------------------------------------------------------
// Signal wiring
// ---------------------------------------------------------------------------

void CommitDialog::wireConnections()
{
    // ---- View interactions -----------------------------------------------
    connect(unstagedView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &CommitDialog::onUnstagedSelectionChanged);
    connect(stagedView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &CommitDialog::onStagedSelectionChanged);

    connect(unstagedView_, &QAbstractItemView::activated,
            this, &CommitDialog::onUnstagedActivated);
    connect(stagedView_, &QAbstractItemView::activated,
            this, &CommitDialog::onStagedActivated);

    // ---- Filter inputs ---------------------------------------------------
    connect(unstagedFilter_, &QLineEdit::textChanged, this,
            [this](const QString& t) {
        unstagedProxy_->setFilterFixedString(t);
    });
    connect(stagedFilter_, &QLineEdit::textChanged, this,
            [this](const QString& t) {
        stagedProxy_->setFilterFixedString(t);
    });

    // ---- File-list buttons -----------------------------------------------
    connect(stageBtn_,    &QPushButton::clicked, this, &CommitDialog::onStageSelected);
    connect(unstageBtn_,  &QPushButton::clicked, this, &CommitDialog::onUnstageSelected);
    connect(discardBtn_,  &QPushButton::clicked, this, &CommitDialog::onDiscardSelected);

    connect(stageAllBtn_, &QPushButton::clicked, svc_, &services::GitService::stageAll);
    connect(unstageAllBtn_, &QPushButton::clicked, svc_, &services::GitService::unstageAll);

    // ---- Hunk / line staging from the diff pane ----------------------------
    // The diff viewer's context menu emits these; the verb (stage
    // vs unstage) is whatever mode showDiffForUnstaged/Staged armed.
    // Patches are built from the exact DiffFileEntry on display, so
    // a stale diff (file changed since render) fails cleanly in
    // `git apply --cached` and surfaces via operationFailed.
    connect(diffView_, &widgets::DiffViewerWidget::hunkActionRequested,
            this, [this](int hunkIdx) {
        if (!diffView_->hasFile() || hunkIdx < 0) return;
        const bool reverse =
            diffView_->hunkActionMode()
            == widgets::DiffViewerWidget::HunkAction::Unstage;
        const QString path =
            QString::fromStdString(diffView_->currentFile().path());
        const std::string patch = git::buildHunkPatch(
            diffView_->currentFile(), static_cast<size_t>(hunkIdx));
        if (patch.empty()) return;
        svc_->applyPatchToIndex(QString::fromStdString(patch), reverse);
        // Re-render the same file/side so the user can keep
        // picking hunks; if nothing of it remains on this side the
        // lookup misses and the pane clears.
        if (reverse) showDiffForStaged(path);
        else         showDiffForUnstaged(path);
    });
    connect(diffView_, &widgets::DiffViewerWidget::linesActionRequested,
            this, [this](int hunkIdx, QList<int> lineIdxs) {
        if (!diffView_->hasFile() || hunkIdx < 0) return;
        const bool reverse =
            diffView_->hunkActionMode()
            == widgets::DiffViewerWidget::HunkAction::Unstage;
        const QString path =
            QString::fromStdString(diffView_->currentFile().path());
        std::set<size_t> lines;
        for (int l : lineIdxs)
            if (l >= 0) lines.insert(static_cast<size_t>(l));
        const std::string patch = git::buildLinesPatch(
            diffView_->currentFile(), static_cast<size_t>(hunkIdx),
            lines);
        if (patch.empty()) {
            QMessageBox::information(this, tr("Stage Lines"),
                tr("The selection doesn't produce a stageable "
                   "change — select at least one added or removed "
                   "line (hunks that end without a newline only "
                   "support whole-hunk staging)."));
            return;
        }
        svc_->applyPatchToIndex(QString::fromStdString(patch), reverse);
        if (reverse) showDiffForStaged(path);
        else         showDiffForUnstaged(path);
    });

    // ---- Commit buttons --------------------------------------------------
    connect(commitBtn_,     &QPushButton::clicked, this, &CommitDialog::onCommitClicked);
    connect(commitPushBtn_, &QPushButton::clicked, this, &CommitDialog::onCommitAndPushClicked);

    // Ctrl+Enter to commit (matches the previous CommitEditorWidget shortcut).
    commitShortcut_ = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(commitShortcut_, &QShortcut::activated, this, &CommitDialog::onCommitClicked);

    // ---- GitService -> dialog --------------------------------------------
    connect(svc_, &services::GitService::statusReady,
            this, &CommitDialog::onStatusReady);
    connect(svc_, &services::GitService::commitComplete,
            this, &CommitDialog::onCommitComplete);
    connect(svc_, &services::GitService::operationFailed,
            this, &CommitDialog::onOperationFailed);

    // Branch list refreshes after every checkout / refresh / open. Use it
    // as our "the current branch may have changed" signal so the title
    // stays in sync with reality.
    connect(svc_, &services::GitService::branchesReady, this,
            [this](std::vector<gitbolt::git::BranchInfo>) { updateTitle(); });
    connect(svc_, &services::GitService::repositoryOpened, this,
            [this](const QString&) {
                readCommitterFromConfig();
                updateTitle();
            });
}

// ---------------------------------------------------------------------------
// Title ("<repoName> - Commit to <branch>")
// ---------------------------------------------------------------------------

void CommitDialog::updateTitle()
{
    if (!svc_ || !svc_->isOpen()) {
        setWindowTitle(tr("Commit"));
        return;
    }

    // One locked read for everything the title needs — the dialog
    // must not touch the Repository outside the service's repo lock
    // (background refresh workers share the libgit2 handle).
    struct TitleInfo {
        QString workdir;
        QString branch;
    };
    const TitleInfo info = svc_->withRepository(
        [this](git::Repository& r) {
            TitleInfo t;
            t.workdir = QString::fromStdString(r.workdir());
            if (auto branchRes = r.headBranchName(); branchRes.ok())
                t.branch = QString::fromStdString(branchRes.value());
            else if (r.isHeadDetached())
                t.branch = tr("(detached HEAD)");
            else
                t.branch = tr("(no branch)");
            return t;
        });

    repoName_ = QDir(info.workdir).dirName();
    if (repoName_.isEmpty())
        repoName_ = QStringLiteral("GitBolt");
    currentBranch_ = info.branch;

    setWindowTitle(tr("%1 - Commit to %2").arg(repoName_, currentBranch_));

    if (branchStatusLabel_) {
        branchStatusLabel_->setText(currentBranch_);
    }
}

// ---------------------------------------------------------------------------
// Committer info from libgit2 config
// ---------------------------------------------------------------------------

void CommitDialog::readCommitterFromConfig()
{
    if (!committerLabel_) return;
    if (!svc_ || !svc_->isOpen()) {
        committerLabel_->setText(tr("Committer: (no repository)"));
        return;
    }

    const auto [name, email] = svc_->withRepository(
        [](git::Repository& r) {
            auto cfg = r.config();
            return std::make_pair(cfg.userName(), cfg.userEmail());
        });

    if (name && email) {
        committerLabel_->setText(tr("Committer: %1 <%2>")
            .arg(QString::fromStdString(*name),
                 QString::fromStdString(*email)));
    } else if (name) {
        committerLabel_->setText(tr("Committer: %1")
            .arg(QString::fromStdString(*name)));
    } else if (email) {
        committerLabel_->setText(tr("Committer: <%1>")
            .arg(QString::fromStdString(*email)));
    } else {
        committerLabel_->setText(tr("Committer: (user.name / user.email not set)"));
    }
}

// ---------------------------------------------------------------------------
// Status entries -> models -> labels
// ---------------------------------------------------------------------------

void CommitDialog::onStatusReady(std::vector<gitbolt::git::StatusEntry> entries)
{
    unstagedModel_->setEntries(entries);
    stagedModel_->setEntries(std::move(entries));

    lastUnstagedCount_ = unstagedModel_->rowCount();
    lastStagedCount_   = stagedModel_->rowCount();

    unstagedLabel_->setText(tr("Unstaged Changes (%1)").arg(lastUnstagedCount_));
    stagedLabel_->setText(  tr("Staged Changes (%1)").arg(lastStagedCount_));

    updateStatusBar();

    // Re-evaluate per-row button enable state in case the previously
    // selected file was just staged/unstaged out from under us.
    onUnstagedSelectionChanged();
    onStagedSelectionChanged();
}

void CommitDialog::updateStatusBar()
{
    if (stagedCountLabel_) {
        const int total = lastUnstagedCount_ + lastStagedCount_;
        stagedCountLabel_->setText(
            tr("Staged %1 / %2").arg(lastStagedCount_).arg(total));
    }
}

// ---------------------------------------------------------------------------
// Selection -> diff
// ---------------------------------------------------------------------------

QStringList CommitDialog::selectedUnstagedPaths() const
{
    // Use selectedIndexes() (not selectedRows(col)) because QListView
    // with setModelColumn(Path) selects single-column indices and
    // selectedRows() returns nothing when no column is "primary". A
    // single-row selection still produces one index here, and a
    // multi-row selection produces one index per row, so deduping
    // by row is sufficient.
    QStringList paths;
    QSet<int> seenRows;
    const auto idxs = unstagedView_->selectionModel()->selectedIndexes();
    for (const auto& proxyIdx : idxs) {
        const auto srcIdx = unstagedProxy_->mapToSource(proxyIdx);
        const int row = srcIdx.row();
        if (seenRows.contains(row)) continue;
        seenRows.insert(row);
        const QString p = unstagedModel_->pathAt(row);
        if (!p.isEmpty()) paths.append(p);
    }
    return paths;
}

QStringList CommitDialog::selectedStagedPaths() const
{
    QStringList paths;
    QSet<int> seenRows;
    const auto idxs = stagedView_->selectionModel()->selectedIndexes();
    for (const auto& proxyIdx : idxs) {
        const auto srcIdx = stagedProxy_->mapToSource(proxyIdx);
        const int row = srcIdx.row();
        if (seenRows.contains(row)) continue;
        seenRows.insert(row);
        const QString p = stagedModel_->pathAt(row);
        if (!p.isEmpty()) paths.append(p);
    }
    return paths;
}

void CommitDialog::onUnstagedSelectionChanged()
{
    const QStringList paths = selectedUnstagedPaths();
    const bool hasSel = !paths.isEmpty();
    stageBtn_->setEnabled(hasSel);
    discardBtn_->setEnabled(hasSel);

    // When a single file is selected, show its diff. Multi-select
    // leaves the previous diff in place — picking the "right" one to
    // show is a UX call; for now we just don't update the diff pane.
    if (paths.size() == 1)
        showDiffForUnstaged(paths.first());
    else if (paths.isEmpty() && stagedView_->selectionModel()->selectedIndexes().isEmpty())
        clearDiff();

    // Selecting in the unstaged list clears the staged-list selection
    // (and vice versa) — so the diff pane always reflects exactly one
    // file from one of the two lists.
    if (hasSel)
        stagedView_->selectionModel()->clearSelection();
}

void CommitDialog::onStagedSelectionChanged()
{
    const QStringList paths = selectedStagedPaths();
    const bool hasSel = !paths.isEmpty();
    unstageBtn_->setEnabled(hasSel);

    if (paths.size() == 1)
        showDiffForStaged(paths.first());
    else if (paths.isEmpty() && unstagedView_->selectionModel()->selectedIndexes().isEmpty())
        clearDiff();

    if (hasSel)
        unstagedView_->selectionModel()->clearSelection();
}

void CommitDialog::onUnstagedActivated(const QModelIndex& index)
{
    if (!index.isValid()) return;
    const auto srcIdx = unstagedProxy_->mapToSource(index);
    const QString path = unstagedModel_->pathAt(srcIdx.row());
    if (!path.isEmpty())
        svc_->stageFile(path);
}

void CommitDialog::onStagedActivated(const QModelIndex& index)
{
    if (!index.isValid()) return;
    const auto srcIdx = stagedProxy_->mapToSource(index);
    const QString path = stagedModel_->pathAt(srcIdx.row());
    if (!path.isEmpty())
        svc_->unstageFile(path);
}

// ---------------------------------------------------------------------------
// Diff fetching
// ---------------------------------------------------------------------------

void CommitDialog::showDiffForUnstaged(const QString& path)
{
    if (!svc_ || !svc_->isOpen() || !diffView_) return;

    auto res = svc_->withRepository(
        [](git::Repository& r) { return r.diffIndexToWorkdir(); });
    if (!res.ok()) {
        clearDiff();
        return;
    }
    const auto& diff = res.value();
    const std::string needle = path.toStdString();
    for (size_t i = 0; i < diff.files.size(); ++i) {
        if (diff.files[i].path() == needle) {
            diffView_->setDiff(diff, static_cast<int>(i));
            // Workdir→index diff: context menu offers Stage Hunk /
            // Stage Selected Lines (Modified files only).
            diffView_->setHunkActionMode(
                widgets::DiffViewerWidget::HunkAction::Stage);
            return;
        }
    }
    clearDiff();
}

void CommitDialog::showDiffForStaged(const QString& path)
{
    if (!svc_ || !svc_->isOpen() || !diffView_) return;

    auto res = svc_->withRepository(
        [](git::Repository& r) { return r.diffHeadToIndex(); });
    if (!res.ok()) {
        clearDiff();
        return;
    }
    const auto& diff = res.value();
    const std::string needle = path.toStdString();
    for (size_t i = 0; i < diff.files.size(); ++i) {
        if (diff.files[i].path() == needle) {
            diffView_->setDiff(diff, static_cast<int>(i));
            // Index→HEAD diff: context menu offers Unstage Hunk /
            // Unstage Selected Lines.
            diffView_->setHunkActionMode(
                widgets::DiffViewerWidget::HunkAction::Unstage);
            return;
        }
    }
    clearDiff();
}

void CommitDialog::clearDiff()
{
    if (diffView_) diffView_->clear();
}

// ---------------------------------------------------------------------------
// Stage / unstage / discard buttons
// ---------------------------------------------------------------------------

void CommitDialog::onStageSelected()
{
    for (const QString& p : selectedUnstagedPaths())
        svc_->stageFile(p);
}

void CommitDialog::onUnstageSelected()
{
    for (const QString& p : selectedStagedPaths())
        svc_->unstageFile(p);
}

void CommitDialog::onDiscardSelected()
{
    const QStringList paths = selectedUnstagedPaths();
    if (paths.isEmpty()) return;

    const QString detail = paths.size() == 1
        ? tr("Discard all uncommitted changes to %1?\n\nThis cannot be undone.")
            .arg(paths.first())
        : tr("Discard all uncommitted changes to %1 files?\n\nThis cannot be undone.")
            .arg(paths.size());

    const auto answer = QMessageBox::question(this,
        tr("Discard changes?"), detail,
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) return;

    for (const QString& p : paths)
        svc_->discardFile(p);
}

// ---------------------------------------------------------------------------
// Commit / Commit & Push
// ---------------------------------------------------------------------------

void CommitDialog::onCommitClicked()
{
    const QString msg = messageEdit_->toPlainText().trimmed();
    if (msg.isEmpty()) {
        errorLabel_->setText(tr("Commit message cannot be empty."));
        errorLabel_->show();
        return;
    }
    errorLabel_->hide();
    pendingCommit_ = true;
    commitBtn_->setEnabled(false);
    commitPushBtn_->setEnabled(false);
    svc_->commitChanges(msg, amendCheck_->isChecked());
}

void CommitDialog::onCommitAndPushClicked()
{
    // Commit, then push. The push half fires as
    // pushAfterCommitRequested once commitComplete reports success —
    // never before, so a failed or rejected commit pushes nothing.
    pushAfterCommit_ = true;
    onCommitClicked();
    if (!pendingCommit_)
        pushAfterCommit_ = false;  // validation failed, nothing queued
}

void CommitDialog::onCommitComplete(bool success, const QString& message)
{
    if (!pendingCommit_) return;
    pendingCommit_ = false;
    commitBtn_->setEnabled(true);
    commitPushBtn_->setEnabled(true);

    if (success) {
        const bool pushRequested = pushAfterCommit_;
        pushAfterCommit_ = false;
        messageEdit_->clear();
        amendCheck_->setChecked(false);
        errorLabel_->hide();
        close();
        if (pushRequested)
            emit pushAfterCommitRequested();
    } else {
        pushAfterCommit_ = false;
        errorLabel_->setText(tr("Commit failed: %1").arg(message));
        errorLabel_->show();
    }
}

void CommitDialog::onOperationFailed(const QString& op, const QString& err)
{
    if (op.startsWith(QLatin1String("commit"),  Qt::CaseInsensitive) ||
        op.startsWith(QLatin1String("stage"),   Qt::CaseInsensitive) ||
        op.startsWith(QLatin1String("unstage"), Qt::CaseInsensitive) ||
        op.startsWith(QLatin1String("discard"), Qt::CaseInsensitive)) {
        errorLabel_->setText(op + tr(" failed: ") + err);
        errorLabel_->show();
        commitBtn_->setEnabled(true);
        commitPushBtn_->setEnabled(true);
        pendingCommit_ = false;
        pushAfterCommit_ = false;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void CommitDialog::refresh()
{
    if (svc_)
        svc_->refreshStatus();
    updateTitle();
    readCommitterFromConfig();
}

void CommitDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);

    if (!restored_ && settings_) {
        // Geometry restore is gated on the "Restore previous dialog
        // size" toggle. Splitter states are always restored — they
        // describe the user's preferred internal layout and aren't
        // really a "size", so the toggle doesn't apply to them.
        if (settings_->restoreLastDialogSize()) {
            const QByteArray geom = settings_->restoreDialogGeometry(
                QString::fromLatin1(kGeometryKey));
            if (!geom.isEmpty()) restoreGeometry(geom);
        }

        const QByteArray mainSplit = settings_->restoreSplitterState(
            QString::fromLatin1(kMainSplitterKey));
        if (!mainSplit.isEmpty()) mainSplitter_->restoreState(mainSplit);

        const QByteArray leftSplit = settings_->restoreSplitterState(
            QString::fromLatin1(kLeftSplitterKey));
        if (!leftSplit.isEmpty()) leftSplitter_->restoreState(leftSplit);

        const QByteArray rightSplit = settings_->restoreSplitterState(
            QString::fromLatin1(kRightSplitterKey));
        if (!rightSplit.isEmpty()) rightSplitter_->restoreState(rightSplit);

        validateGeometryOnScreen();
        restored_ = true;
    }

    refresh();
}

void CommitDialog::closeEvent(QCloseEvent* e)
{
    if (settings_) {
        settings_->saveDialogGeometry(
            QString::fromLatin1(kGeometryKey), saveGeometry());
        settings_->saveSplitterState(
            QString::fromLatin1(kMainSplitterKey), mainSplitter_->saveState());
        settings_->saveSplitterState(
            QString::fromLatin1(kLeftSplitterKey), leftSplitter_->saveState());
        settings_->saveSplitterState(
            QString::fromLatin1(kRightSplitterKey), rightSplitter_->saveState());
    }
    QDialog::closeEvent(e);
}

// If the dialog was last closed on a monitor that is no longer attached,
// restoreGeometry will happily place it off-screen and the user won't be
// able to see it. Detect that case and fall back to a centered-on-parent
// layout.
void CommitDialog::validateGeometryOnScreen()
{
    const QPoint center = frameGeometry().center();
    if (QGuiApplication::screenAt(center) != nullptr)
        return;

    // Off-screen recovery — fall back to the user's configured
    // global dialog default and re-center over the parent.
    resize(conf::SettingsService::loadDefaultDialogSize());
    if (auto* p = parentWidget()) {
        const QRect pg = p->geometry();
        move(pg.center().x() - width() / 2,
             pg.center().y() - height() / 2);
    }
}

} // namespace gitbolt::dialogs
