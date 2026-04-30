#include "dialogs/SettingsDialog.h"
#include "conf/SettingsService.h"
#include "conf/ThemeService.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QButtonGroup>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

namespace {

// Visual tuning constants. Picked to match the GitExtensions
// reference screenshot proportions at a 16pt body font.
constexpr int kNavWidth        = 220;
constexpr int kDialogWidth     = 860;
constexpr int kDialogHeight    = 600;

// Indent the category header slightly so sub-section rows sit
// clearly under it without looking flat.
QTreeWidgetItem* makeCategoryItem(QTreeWidget* tree, const QString& label)
{
    auto* item = new QTreeWidgetItem(tree, QStringList{label});
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
    // Category rows are NOT selectable — only their sub-sections
    // have pages. Clicking the category expands/collapses it.
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    item->setExpanded(true);
    return item;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SettingsDialog::SettingsDialog(conf::SettingsService* settings,
                               conf::ThemeService* theme,
                               QWidget* parent)
    : QDialog(parent)
    , settings_(settings)
    , theme_(theme)
{
    setWindowTitle(tr("Settings"));
    resize(kDialogWidth, kDialogHeight);

    // ---- Root layout: tree | pages on top, buttons on bottom ----
    auto* root = new QVBoxLayout(this);

    auto* split = new QHBoxLayout;
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(10);

    // --- Left: navigation tree ---
    nav_ = new QTreeWidget(this);
    nav_->setHeaderHidden(true);
    nav_->setRootIsDecorated(true);
    nav_->setFixedWidth(kNavWidth);
    nav_->setIndentation(16);
    nav_->setAlternatingRowColors(true);
    nav_->setFocusPolicy(Qt::StrongFocus);
    split->addWidget(nav_);

    // --- Right: stacked pages ---
    pages_ = new QStackedWidget(this);
    split->addWidget(pages_, 1);

    root->addLayout(split, 1);

    // ---- Populate: three top-level categories with flat leaves ----
    //
    // Category layout (strictly two levels deep — the user asked
    // for "no sub-sub sections"):
    //
    //   GitBolt                ← category (bold, not selectable)
    //     General
    //     UI Design            ← new: bottom-pane ratio setting
    //     Appearance
    //     Shortcuts
    //   Git
    //     Git Config
    //   Plugins
    //     Manage Plugins
    //
    // The "GitBolt" / "Git" / "Plugins" split mirrors GitExtensions'
    // "Git Extensions / Git / Plugins" three-category layout.
    auto* catGitBolt  = makeCategoryItem(nav_, tr("GitBolt"));
    QTreeWidgetItem* firstLeaf =
        addSubsection(catGitBolt,  tr("General"),   createGeneralPage());
    addSubsection(catGitBolt,  tr("UI Design"),  createUiDesignPage());
    addSubsection(catGitBolt,  tr("Appearance"), createAppearancePage());
    addSubsection(catGitBolt,  tr("Shortcuts"),  createShortcutsPage());
    addSubsection(catGitBolt,  tr("Recent Repositories"),
                  createRecentReposPage());

    auto* catGit      = makeCategoryItem(nav_, tr("Git"));
    addSubsection(catGit,      tr("Git Config"),    createGitConfigPage());

    auto* catPlugins  = makeCategoryItem(nav_, tr("Plugins"));
    addSubsection(catPlugins,  tr("Manage Plugins"), createPluginsPage());

    // Expand everything so users see all sub-sections immediately.
    nav_->expandAll();

    // Select the first real leaf (General) by default so the
    // dialog opens on a non-empty page.
    if (firstLeaf) {
        nav_->setCurrentItem(firstLeaf);
    }

    connect(nav_, &QTreeWidget::currentItemChanged,
            this, &SettingsDialog::onTreeSelectionChanged);

    // ---- Buttons ----
    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    root->addWidget(buttons_);

    connect(buttons_, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::apply);

    loadSettings();
    loadGitConfig();
}

// ---------------------------------------------------------------------------
// Tree <-> Stack mapping
// ---------------------------------------------------------------------------

QTreeWidgetItem* SettingsDialog::addSubsection(QTreeWidgetItem* parent,
                                               const QString& label,
                                               QWidget* page)
{
    auto* item = new QTreeWidgetItem(parent, QStringList{label});
    const int index = pages_->addWidget(page);
    // Stash the stack index so the selection handler can look it
    // up in O(1) — no need for a separate map.
    item->setData(0, Qt::UserRole, index);
    return item;
}

void SettingsDialog::onTreeSelectionChanged()
{
    auto* item = nav_->currentItem();
    if (!item)
        return;

    // Category rows are un-selectable, so currentItem() will always
    // be a leaf here. Defensive check anyway.
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid())
        return;

    const int idx = v.toInt();
    if (idx >= 0 && idx < pages_->count())
        pages_->setCurrentIndex(idx);
}

// ---------------------------------------------------------------------------
// General page
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createGeneralPage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    auto* fontRow = new QHBoxLayout;
    fontCombo_ = new QFontComboBox(page);
    fontCombo_->setFontFilters(QFontComboBox::MonospacedFonts);
    fontSizeSpin_ = new QSpinBox(page);
    fontSizeSpin_->setRange(6, 72);
    fontRow->addWidget(fontCombo_, 1);
    fontRow->addWidget(fontSizeSpin_);
    form->addRow(tr("Code Font:"), fontRow);

    tabSizeSpin_ = new QSpinBox(page);
    tabSizeSpin_->setRange(1, 16);
    form->addRow(tr("Tab Size:"), tabSizeSpin_);

    showWhitespaceCheck_ = new QCheckBox(tr("Show whitespace characters"), page);
    form->addRow(QString(), showWhitespaceCheck_);

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return page;
}

// ---------------------------------------------------------------------------
// UI Design page — window layout defaults
// ---------------------------------------------------------------------------
//
// Currently one setting: the default height of the bottom inspector
// pane (Commit / Diff / File tree / GPG / Console) as a percent of
// the revision-graph column's height. The slider and spinbox are
// two-way bound so touching either updates the other, and the small
// label underneath shows a live "Revision graph: 60%  Inspector: 40%"
// summary so users can see what the number means before applying.

QWidget* SettingsDialog::createUiDesignPage()
{
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);

    auto* layoutGroup = new QGroupBox(tr("Default Pane Sizes"), page);
    auto* form = new QFormLayout(layoutGroup);

    // --- Bottom inspector pane percent --------------------------
    auto* row = new QHBoxLayout;
    bottomPaneSlider_ = new QSlider(Qt::Horizontal, layoutGroup);
    bottomPaneSlider_->setRange(10, 90);
    bottomPaneSlider_->setTickInterval(10);
    bottomPaneSlider_->setTickPosition(QSlider::TicksBelow);

    bottomPaneSpin_ = new QSpinBox(layoutGroup);
    bottomPaneSpin_->setRange(10, 90);
    bottomPaneSpin_->setSuffix(QStringLiteral(" %"));
    bottomPaneSpin_->setFixedWidth(80);

    row->addWidget(bottomPaneSlider_, 1);
    row->addWidget(bottomPaneSpin_, 0);

    form->addRow(tr("Bottom inspector pane:"), row);

    bottomPanePreview_ = new QLabel(layoutGroup);
    bottomPanePreview_->setStyleSheet(
        QStringLiteral("QLabel { color: palette(mid); font-size: 11pt; }"));
    form->addRow(QString(), bottomPanePreview_);

    auto* note = new QLabel(
        tr("This is the DEFAULT height of the commit inspector panel "
           "(Commit / Diff / File tree / GPG / Console) at the bottom "
           "of the repository view. You can still drag the splitter "
           "at any time to override this."),
        layoutGroup);
    note->setWordWrap(true);
    note->setStyleSheet(
        QStringLiteral("QLabel { color: palette(mid); }"));
    form->addRow(QString(), note);

    root->addWidget(layoutGroup);
    root->addStretch();

    // Two-way binding — avoid infinite ping-pong by checking that
    // the value actually changed before forwarding.
    connect(bottomPaneSlider_, &QSlider::valueChanged,
            bottomPaneSpin_, [this](int v) {
                if (bottomPaneSpin_->value() != v)
                    bottomPaneSpin_->setValue(v);
            });
    connect(bottomPaneSpin_, qOverload<int>(&QSpinBox::valueChanged),
            bottomPaneSlider_, [this](int v) {
                if (bottomPaneSlider_->value() != v)
                    bottomPaneSlider_->setValue(v);
            });

    // Live preview label update.
    auto updatePreview = [this](int pct) {
        if (bottomPanePreview_)
            bottomPanePreview_->setText(
                tr("Revision graph: %1%   •   Inspector: %2%")
                    .arg(100 - pct).arg(pct));
    };
    connect(bottomPaneSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, updatePreview);

    // Seed from current settings (loadSettings() will overwrite
    // again once the dialog is fully constructed, but setting
    // early avoids a visible 0% flash on slow hardware).
    // Keep the fallback in sync with SettingsService::bottomPanePercent()'s
    // default — both should show the same value when there's no saved pref.
    const int initial = settings_ ? settings_->bottomPanePercent() : 45;
    bottomPaneSlider_->setValue(initial);
    bottomPaneSpin_->setValue(initial);
    updatePreview(initial);

    return page;
}

// ---------------------------------------------------------------------------
// Git config page
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createGitConfigPage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    auto* identityGroup = new QGroupBox(tr("Identity"), page);
    auto* identityForm = new QFormLayout(identityGroup);
    userNameEdit_ = new QLineEdit(identityGroup);
    userNameEdit_->setPlaceholderText(tr("Your Name"));
    identityForm->addRow(tr("user.name:"), userNameEdit_);
    userEmailEdit_ = new QLineEdit(identityGroup);
    userEmailEdit_->setPlaceholderText(tr("you@example.com"));
    identityForm->addRow(tr("user.email:"), userEmailEdit_);
    form->addRow(identityGroup);

    auto* remoteGroup = new QGroupBox(tr("Defaults"), page);
    auto* remoteForm = new QFormLayout(remoteGroup);
    defaultRemoteEdit_ = new QLineEdit(remoteGroup);
    defaultRemoteEdit_->setPlaceholderText(QStringLiteral("origin"));
    remoteForm->addRow(tr("Default Remote:"), defaultRemoteEdit_);
    form->addRow(remoteGroup);

    auto* credGroup = new QGroupBox(tr("Credentials"), page);
    auto* credForm = new QFormLayout(credGroup);
    credentialHelperEdit_ = new QLineEdit(credGroup);
    credentialHelperEdit_->setReadOnly(true);
    credentialHelperEdit_->setPlaceholderText(tr("(not configured)"));
    credForm->addRow(tr("Credential Helper:"), credentialHelperEdit_);
    form->addRow(credGroup);

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return page;
}

// ---------------------------------------------------------------------------
// Appearance page
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createAppearancePage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* form = new QFormLayout;
    themeCombo_ = new QComboBox(page);
    if (theme_)
        themeCombo_->addItems(theme_->availableThemes());
    form->addRow(tr("Theme:"), themeCombo_);
    layout->addLayout(form);

    // Preview area showing sampled palette colors
    auto* previewGroup = new QGroupBox(tr("Theme Preview"), page);
    auto* previewLayout = new QVBoxLayout(previewGroup);

    previewArea_ = new QWidget(previewGroup);
    previewArea_->setMinimumHeight(100);
    previewArea_->setAutoFillBackground(true);

    auto updatePreview = [this]() {
        if (!previewArea_)
            return;
        QPalette pal = QApplication::palette();
        QPalette p;
        QString themeName = themeCombo_->currentText();
        if (themeName == QStringLiteral("Dark")) {
            p.setColor(QPalette::Window, QColor(30, 30, 30));
            p.setColor(QPalette::WindowText, QColor(212, 212, 212));
            p.setColor(QPalette::Base, QColor(25, 25, 25));
            p.setColor(QPalette::Highlight, QColor(0, 120, 212));
        } else if (themeName == QStringLiteral("Light")) {
            p = QApplication::style()->standardPalette();
        } else {
            p = pal;
        }
        previewArea_->setPalette(p);
        previewArea_->update();
    };

    connect(themeCombo_, &QComboBox::currentTextChanged, this, updatePreview);
    previewLayout->addWidget(previewArea_);

    // Show small colored rectangles
    auto* sampleRow = new QHBoxLayout;
    auto addSwatch = [&](const QString& label, QPalette::ColorRole role) {
        auto* frame = new QFrame(page);
        frame->setFixedSize(60, 30);
        frame->setAutoFillBackground(true);
        QPalette fp;
        fp.setColor(QPalette::Window, QApplication::palette().color(role));
        frame->setPalette(fp);
        frame->setFrameStyle(QFrame::Box);
        auto* col = new QVBoxLayout;
        col->setSpacing(2);
        col->addWidget(frame, 0, Qt::AlignCenter);
        col->addWidget(new QLabel(label, page), 0, Qt::AlignCenter);
        sampleRow->addLayout(col);
    };
    addSwatch(tr("Window"), QPalette::Window);
    addSwatch(tr("Base"), QPalette::Base);
    addSwatch(tr("Text"), QPalette::Text);
    addSwatch(tr("Highlight"), QPalette::Highlight);
    addSwatch(tr("Button"), QPalette::Button);
    previewLayout->addLayout(sampleRow);

    layout->addWidget(previewGroup);
    layout->addStretch();
    return page;
}

// ---------------------------------------------------------------------------
// Shortcuts page
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createShortcutsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    shortcutsTable_ = new QTableWidget(page);
    shortcutsTable_->setColumnCount(2);
    shortcutsTable_->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut")});
    shortcutsTable_->horizontalHeader()->setStretchLastSection(true);
    shortcutsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    shortcutsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    shortcutsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(shortcutsTable_);

    connect(shortcutsTable_, &QTableWidget::cellDoubleClicked,
            this, &SettingsDialog::onShortcutCellDoubleClicked);

    // Key sequence edit (hidden until double-click activates it)
    keySeqEdit_ = new QKeySequenceEdit(page);
    keySeqEdit_->hide();
    connect(keySeqEdit_, &QKeySequenceEdit::editingFinished, this, [this]() {
        int row = shortcutsTable_->currentRow();
        if (row >= 0 && row < shortcutsTable_->rowCount()) {
            auto* item = shortcutsTable_->item(row, 1);
            if (item)
                item->setText(keySeqEdit_->keySequence().toString(QKeySequence::NativeText));
        }
        keySeqEdit_->hide();
    });

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto* resetBtn = new QPushButton(tr("Reset to Default"), page);
    connect(resetBtn, &QPushButton::clicked, this, &SettingsDialog::resetShortcutsToDefault);
    btnRow->addWidget(resetBtn);
    layout->addLayout(btnRow);

    populateShortcutsTable();
    return page;
}

// ---------------------------------------------------------------------------
// Plugins page — placeholder until we have a real plugin host
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createPluginsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(tr("Plugins"), page);
    QFont f = title->font();
    f.setPointSize(14);
    f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    auto* body = new QLabel(
        tr("GitBolt's plugin system is on the roadmap but isn't wired "
           "up yet. This page is a placeholder — no action is needed."),
        page);
    body->setWordWrap(true);
    body->setStyleSheet(QStringLiteral("QLabel { color: palette(mid); }"));
    layout->addWidget(body);

    layout->addStretch();
    return page;
}

// ---------------------------------------------------------------------------
// Shortcut helpers
// ---------------------------------------------------------------------------

void SettingsDialog::onShortcutCellDoubleClicked(int row, int column)
{
    Q_UNUSED(column);
    if (row < 0 || row >= shortcutsTable_->rowCount())
        return;

    // Position the QKeySequenceEdit over the shortcut cell
    auto* item = shortcutsTable_->item(row, 1);
    if (!item)
        return;

    QRect cellRect = shortcutsTable_->visualItemRect(item);
    QPoint pos = shortcutsTable_->viewport()->mapTo(this, cellRect.topLeft());

    keySeqEdit_->setKeySequence(QKeySequence::fromString(item->text(), QKeySequence::NativeText));
    keySeqEdit_->setGeometry(pos.x(), pos.y(), cellRect.width(), cellRect.height());
    keySeqEdit_->show();
    keySeqEdit_->setFocus();
}

void SettingsDialog::resetShortcutsToDefault()
{
    populateShortcutsTable();
}

void SettingsDialog::populateShortcutsTable()
{
    // Populate with common application shortcuts.
    // In a full implementation these would come from a ShortcutManager; for now
    // we list representative defaults.
    struct ShortcutEntry {
        const char* name;
        const char* shortcut;
    };
    static const ShortcutEntry defaults[] = {
        {"Open Repository",     "Ctrl+O"},
        {"Close Repository",    "Ctrl+W"},
        {"Refresh",             "F5"},
        {"Commit",              "Ctrl+Return"},
        {"Stage File",          "Ctrl+Shift+S"},
        {"Unstage File",        "Ctrl+Shift+U"},
        {"Push",                "Ctrl+Shift+P"},
        {"Pull",                "Ctrl+Shift+L"},
        {"Fetch",               "Ctrl+Shift+F"},
        {"Find / Search",       "Ctrl+F"},
        {"Settings",            "Ctrl+,"},
        {"Toggle Console",      "Ctrl+`"},
    };

    shortcutsTable_->setRowCount(static_cast<int>(std::size(defaults)));
    for (int i = 0; i < static_cast<int>(std::size(defaults)); ++i) {
        auto* nameItem = new QTableWidgetItem(tr(defaults[i].name));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        shortcutsTable_->setItem(i, 0, nameItem);

        QString saved = settings_
            ? settings_->value(QStringLiteral("shortcuts/%1").arg(QLatin1StringView(defaults[i].name)),
                               QLatin1StringView(defaults[i].shortcut)).toString()
            : QLatin1StringView(defaults[i].shortcut);
        auto* keyItem = new QTableWidgetItem(saved);
        keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
        shortcutsTable_->setItem(i, 1, keyItem);
    }
}

// ---------------------------------------------------------------------------
// Recent Repositories page
// ---------------------------------------------------------------------------
//
// Mirrors the GitExtensions "Recent repositories settings" dialog,
// scoped to the options we actually support. We omit the "top
// repositories" section (separate pinned-favorites feature that
// hasn't been built yet) and the "combobox minimum width" option
// (we render recents as a table on the dashboard, not a combobox —
// the column widths are already user-draggable via the header).

QWidget* SettingsDialog::createRecentReposPage()
{
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    // --- Left column: the knobs ---
    auto* leftCol = new QVBoxLayout;
    leftCol->setSpacing(8);

    // Max count row.
    auto* countRow = new QHBoxLayout;
    auto* countLabel = new QLabel(tr("Maximum number of recent repositories:"), page);
    recentMaxCountSpin_ = new QSpinBox(page);
    recentMaxCountSpin_->setRange(1, 200);
    recentMaxCountSpin_->setFixedWidth(80);
    countRow->addWidget(countLabel, 1);
    countRow->addWidget(recentMaxCountSpin_, 0);
    leftCol->addLayout(countRow);

    // Alphabetical sort.
    recentSortCheck_ = new QCheckBox(
        tr("Sort recent repositories alphabetically"), page);
    leftCol->addWidget(recentSortCheck_);

    // Shortening strategy group.
    auto* shortenGroup = new QGroupBox(tr("Shortening strategy"), page);
    auto* shortenLayout = new QVBoxLayout(shortenGroup);
    recentShortenNone_   = new QRadioButton(tr("Do not shorten"), shortenGroup);
    recentShortenMiddle_ = new QRadioButton(
        tr("Replace middle part with dots"), shortenGroup);
    recentShortenSigDir_ = new QRadioButton(
        tr("Show the most significant directory"), shortenGroup);
    shortenLayout->addWidget(recentShortenNone_);
    shortenLayout->addWidget(recentShortenMiddle_);
    shortenLayout->addWidget(recentShortenSigDir_);

    // Group them so mutual exclusion is explicit (they'd be mutually
    // exclusive anyway as direct QRadioButton siblings, but an
    // explicit button group makes intent obvious and survives any
    // future reparenting).
    auto* shortenButtons = new QButtonGroup(page);
    shortenButtons->addButton(recentShortenNone_,
        static_cast<int>(conf::SettingsService::RecentShortening::None));
    shortenButtons->addButton(recentShortenMiddle_,
        static_cast<int>(conf::SettingsService::RecentShortening::MiddleEllipsis));
    shortenButtons->addButton(recentShortenSigDir_,
        static_cast<int>(conf::SettingsService::RecentShortening::SignificantDir));

    leftCol->addWidget(shortenGroup);

    auto* note = new QLabel(
        tr("These options control how the Recent Repositories list on "
           "the home screen is ordered and how each path is rendered. "
           "Clearing the list itself is done from the home screen's "
           "\"Clear Recent\" button."),
        page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: palette(mid);"));
    leftCol->addWidget(note);
    leftCol->addStretch();

    // --- Right column: current list preview ---
    auto* rightCol = new QVBoxLayout;
    rightCol->setSpacing(6);
    auto* listHeader = new QLabel(tr("Current recent repositories:"), page);
    rightCol->addWidget(listHeader);
    recentListPreview_ = new QListWidget(page);
    recentListPreview_->setSelectionMode(QAbstractItemView::NoSelection);
    recentListPreview_->setFocusPolicy(Qt::NoFocus);
    rightCol->addWidget(recentListPreview_, 1);

    root->addLayout(leftCol, 1);
    root->addLayout(rightCol, 1);

    return page;
}

// ---------------------------------------------------------------------------
// Load / Save
// ---------------------------------------------------------------------------

void SettingsDialog::loadSettings()
{
    if (!settings_)
        return;

    QFont font = settings_->codeFont();
    fontCombo_->setCurrentFont(font);
    fontSizeSpin_->setValue(settings_->codeFontSize());
    tabSizeSpin_->setValue(settings_->tabSize());
    showWhitespaceCheck_->setChecked(settings_->showWhitespace());
    defaultRemoteEdit_->setText(settings_->defaultRemote());

    // UI Design
    const int pct = settings_->bottomPanePercent();
    if (bottomPaneSlider_) bottomPaneSlider_->setValue(pct);
    if (bottomPaneSpin_)   bottomPaneSpin_->setValue(pct);

    if (theme_) {
        int idx = themeCombo_->findText(theme_->currentTheme());
        if (idx >= 0)
            themeCombo_->setCurrentIndex(idx);
    }

    // Recent Repositories
    if (recentMaxCountSpin_)
        recentMaxCountSpin_->setValue(settings_->maxRecentRepositories());
    if (recentSortCheck_)
        recentSortCheck_->setChecked(settings_->sortRecentAlphabetically());
    if (recentShortenNone_) {
        switch (settings_->recentShorteningStrategy()) {
        case conf::SettingsService::RecentShortening::MiddleEllipsis:
            recentShortenMiddle_->setChecked(true); break;
        case conf::SettingsService::RecentShortening::SignificantDir:
            recentShortenSigDir_->setChecked(true); break;
        case conf::SettingsService::RecentShortening::None:
        default:
            recentShortenNone_->setChecked(true); break;
        }
    }
    if (recentListPreview_) {
        recentListPreview_->clear();
        recentListPreview_->addItems(settings_->recentRepositories());
    }
}

void SettingsDialog::loadGitConfig()
{
    // Read global git config via the git CLI for portability
    auto readConfig = [](const QString& key) -> QString {
        QProcess proc;
        proc.start(QStringLiteral("git"), {QStringLiteral("config"), QStringLiteral("--global"), key});
        proc.waitForFinished(3000);
        return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    };

    userNameEdit_->setText(readConfig(QStringLiteral("user.name")));
    userEmailEdit_->setText(readConfig(QStringLiteral("user.email")));
    credentialHelperEdit_->setText(readConfig(QStringLiteral("credential.helper")));
}

void SettingsDialog::saveGitConfig()
{
    auto writeConfig = [](const QString& key, const QString& value) {
        if (value.isEmpty())
            return;
        QProcess proc;
        proc.start(QStringLiteral("git"),
                    {QStringLiteral("config"), QStringLiteral("--global"), key, value});
        proc.waitForFinished(3000);
    };

    writeConfig(QStringLiteral("user.name"), userNameEdit_->text().trimmed());
    writeConfig(QStringLiteral("user.email"), userEmailEdit_->text().trimmed());
}

void SettingsDialog::apply()
{
    if (!settings_)
        return;

    // General
    QFont font = fontCombo_->currentFont();
    font.setPointSize(fontSizeSpin_->value());
    settings_->setCodeFont(font);
    settings_->setCodeFontSize(fontSizeSpin_->value());
    settings_->setTabSize(tabSizeSpin_->value());
    settings_->setShowWhitespace(showWhitespaceCheck_->isChecked());

    // UI Design — writing this triggers settingsChanged, which
    // RepositoryView is connected to; it re-runs applyBottomPanePercent
    // and snaps the splitter to the new ratio live.
    if (bottomPaneSpin_)
        settings_->setBottomPanePercent(bottomPaneSpin_->value());

    // Git
    settings_->setDefaultRemote(defaultRemoteEdit_->text().trimmed());
    saveGitConfig();

    // Theme
    if (theme_)
        theme_->setTheme(themeCombo_->currentText());

    // Recent Repositories
    if (recentMaxCountSpin_)
        settings_->setMaxRecentRepositories(recentMaxCountSpin_->value());
    if (recentSortCheck_)
        settings_->setSortRecentAlphabetically(recentSortCheck_->isChecked());
    if (recentShortenNone_) {
        using RS = conf::SettingsService::RecentShortening;
        RS strategy = RS::None;
        if (recentShortenMiddle_ && recentShortenMiddle_->isChecked())
            strategy = RS::MiddleEllipsis;
        else if (recentShortenSigDir_ && recentShortenSigDir_->isChecked())
            strategy = RS::SignificantDir;
        settings_->setRecentShorteningStrategy(strategy);
    }

    // Shortcuts
    for (int i = 0; i < shortcutsTable_->rowCount(); ++i) {
        auto* nameItem = shortcutsTable_->item(i, 0);
        auto* keyItem = shortcutsTable_->item(i, 1);
        if (nameItem && keyItem)
            settings_->setValue(QStringLiteral("shortcuts/%1").arg(nameItem->text()),
                                keyItem->text());
    }
}

void SettingsDialog::onAccepted()
{
    apply();
    accept();
}

} // namespace gitbolt::dialogs
