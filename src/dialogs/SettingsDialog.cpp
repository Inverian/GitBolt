#include "dialogs/SettingsDialog.h"

#include <QFutureWatcher>
#include <QScopeGuard>
#include <QtConcurrent>
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

// Visual tuning constant. The dialog's actual width/height come
// from SettingsService::defaultDialogSize() — this is just the
// nav-tree column width inside the dialog.
constexpr int kNavWidth = 220;

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
    // Configured default + per-dialog persistence. Helper handles
    // the toggle ("Restore previous dialog size") + saved-geometry
    // restore + finished-signal hookup for save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "settings");

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
    // Don't let inputs grow to fill the dialog horizontally — the
    // dialog itself can be 1200+ px wide when the user has dragged
    // it large, but a font picker + a size spinbox + a tab-size
    // spinbox don't need anywhere near that. FieldsStayAtSizeHint
    // pins each row's field column to its widget sizeHint, so the
    // space to the right of the inputs stays empty (matching the
    // GitExtensions reference).
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);

    auto* fontRow = new QHBoxLayout;
    fontCombo_ = new QFontComboBox(page);
    fontCombo_->setFontFilters(QFontComboBox::MonospacedFonts);
    // Cap the font picker so long font names don't push the size
    // spinbox off-screen on a narrow dialog. 260 px comfortably
    // fits names up to ~25 chars at the system font size.
    fontCombo_->setMinimumWidth(220);
    fontCombo_->setMaximumWidth(260);
    fontSizeSpin_ = new QSpinBox(page);
    fontSizeSpin_->setRange(6, 72);
    fontSizeSpin_->setMaximumWidth(70);
    fontRow->addWidget(fontCombo_);
    fontRow->addWidget(fontSizeSpin_);
    fontRow->addStretch();
    form->addRow(tr("Code Font:"), fontRow);

    tabSizeSpin_ = new QSpinBox(page);
    tabSizeSpin_->setRange(1, 16);
    tabSizeSpin_->setMaximumWidth(70);
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
    // Pin every row's input column to its sizeHint so spinboxes
    // and sliders don't sprawl across a wide dialog.
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);

    // --- Bottom inspector pane percent --------------------------
    auto* row = new QHBoxLayout;
    bottomPaneSlider_ = new QSlider(Qt::Horizontal, layoutGroup);
    bottomPaneSlider_->setRange(10, 90);
    bottomPaneSlider_->setTickInterval(10);
    bottomPaneSlider_->setTickPosition(QSlider::TicksBelow);
    // 320–480 px keeps the slider usable (enough drag distance for
    // ~5%-per-pixel resolution at the 80-step range) without
    // letting it stretch the full width of a 1500-px dialog.
    bottomPaneSlider_->setMinimumWidth(320);
    bottomPaneSlider_->setMaximumWidth(480);

    bottomPaneSpin_ = new QSpinBox(layoutGroup);
    bottomPaneSpin_->setRange(10, 90);
    bottomPaneSpin_->setSuffix(QStringLiteral(" %"));
    bottomPaneSpin_->setFixedWidth(80);

    row->addWidget(bottomPaneSlider_, 0);
    row->addWidget(bottomPaneSpin_, 0);
    row->addStretch();

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
    // Stretch is added at the very end of the page (after the
    // Default Window Size group) so all the groups stack from the
    // top instead of one floating to the bottom.

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

    // --- Default Window Size --------------------------------------
    // Two modes: remember the previous geometry across launches
    // (the default), or always open at a configured fixed size.
    // The spinboxes are greyed out when "remember previous" is on
    // since the saved geometry takes priority over them anyway.
    auto* sizeGroup = new QGroupBox(tr("Default Window Size"), page);
    auto* sizeForm = new QFormLayout(sizeGroup);
    sizeForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);

    // Width spinboxes show 4-digit values + " px" suffix + the
    // step arrows — 140 px fits all of that with a small margin.
    constexpr int kPxSpinWidth = 140;

    restoreLastSizeCheck_ = new QCheckBox(
        tr("Restore previous window size on launch"), sizeGroup);
    sizeForm->addRow(QString(), restoreLastSizeCheck_);

    // Functional minimum is 1024 × 700 — below that the sidebar,
    // repo view, and inspector tabs start crowding into uselessness.
    // QSpinBox auto-clamps anything typed below the minimum to the
    // minimum on focus-out, so a user typing "500" sees it snap to
    // 1024. Keep these in lockstep with kStartupWidthMin/kStartupHeightMin
    // in SettingsService.cpp.
    constexpr int kMinStartupWidth  = 1024;
    constexpr int kMinStartupHeight = 700;

    // CorrectToNearestValue — when the user types a value out of
    // range and tabs/Enters out, snap to the nearest valid value
    // (which for a sub-min input means snap UP to the minimum).
    // The default CorrectToPreviousValue silently reverts to the
    // last good value, which is confusing — typing 500 then tabbing
    // makes the field appear unchanged with no feedback.
    startupWidthSpin_ = new QSpinBox(sizeGroup);
    startupWidthSpin_->setRange(kMinStartupWidth, 8000);
    startupWidthSpin_->setSuffix(QStringLiteral(" px"));
    startupWidthSpin_->setSingleStep(50);
    startupWidthSpin_->setFixedWidth(kPxSpinWidth);
    startupWidthSpin_->setCorrectionMode(
        QAbstractSpinBox::CorrectToNearestValue);
    sizeForm->addRow(tr("Default width:"), startupWidthSpin_);

    startupHeightSpin_ = new QSpinBox(sizeGroup);
    startupHeightSpin_->setRange(kMinStartupHeight, 5000);
    startupHeightSpin_->setSuffix(QStringLiteral(" px"));
    startupHeightSpin_->setSingleStep(50);
    startupHeightSpin_->setFixedWidth(kPxSpinWidth);
    startupHeightSpin_->setCorrectionMode(
        QAbstractSpinBox::CorrectToNearestValue);
    sizeForm->addRow(tr("Default height:"), startupHeightSpin_);

    auto syncStartupSizeEnabled = [this]() {
        const bool restore = restoreLastSizeCheck_
            && restoreLastSizeCheck_->isChecked();
        if (startupWidthSpin_)  startupWidthSpin_->setEnabled(!restore);
        if (startupHeightSpin_) startupHeightSpin_->setEnabled(!restore);
    };
    connect(restoreLastSizeCheck_, &QCheckBox::toggled,
            this, syncStartupSizeEnabled);

    // Live readout of the parent window's current size — handy for
    // users who want to type their preferred size into the spinboxes
    // and need a starting reference point. Refreshed by
    // refreshCurrentMainWindowSize() in loadSettings() and after
    // Apply runs (since Apply may have just resized the parent).
    currentMainWindowSize_ = new QLabel(sizeGroup);
    currentMainWindowSize_->setStyleSheet(
        QStringLiteral("QLabel { color: palette(mid); }"));
    sizeForm->addRow(QString(), currentMainWindowSize_);

    auto* sizeNote = new QLabel(
        tr("When “Restore previous” is on, the window remembers "
           "wherever you last left it. Otherwise, it always opens at the "
           "configured default size each launch, and Apply resizes the "
           "current window immediately. Minimum size is %1 × %2 px — "
           "values below that snap to the minimum.")
            .arg(kMinStartupWidth).arg(kMinStartupHeight),
        sizeGroup);
    sizeNote->setWordWrap(true);
    sizeNote->setStyleSheet(
        QStringLiteral("QLabel { color: palette(mid); }"));
    sizeForm->addRow(QString(), sizeNote);

    root->addWidget(sizeGroup);

    // Seed values right away so the dialog shows real numbers on
    // open instead of a flash of (0, 0). loadSettings() will rerun
    // these reads once the dialog is fully constructed.
    if (settings_) {
        restoreLastSizeCheck_->setChecked(settings_->restoreLastWindowSize());
        startupWidthSpin_ ->setValue(settings_->startupWindowWidth());
        startupHeightSpin_->setValue(settings_->startupWindowHeight());
    }
    syncStartupSizeEnabled();

    // --- Default Dialog Size --------------------------------------
    // One W × H pair applied to every popup dialog the user can
    // resize (Settings, Commit, Clone, Tag, Stash, Rebase, Reflog,
    // Cherry-Pick, Worktree, Remotes, Text Editor, Stash Manager).
    // Changes apply the next time each dialog opens — we don't
    // resize the running Settings dialog (would jump under the
    // user) or any open Commit dialog. CorrectToNearestValue
    // snaps any sub-min input up to the floor on focus-out.
    //
    // Layout deliberately mirrors the Default Window Size group
    // above — two stacked rows with full-width spinboxes — so the
    // two groups read as a matched pair.
    constexpr int kMinDefaultDialogW = 400;
    constexpr int kMinDefaultDialogH = 300;

    auto* dlgGroup = new QGroupBox(tr("Default Dialog Size"), page);
    auto* dlgForm  = new QFormLayout(dlgGroup);
    dlgForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);

    restoreLastDialogSizeCheck_ = new QCheckBox(
        tr("Restore previous dialog size on open"), dlgGroup);
    dlgForm->addRow(QString(), restoreLastDialogSizeCheck_);

    defaultDialogWidthSpin_ = new QSpinBox(dlgGroup);
    defaultDialogWidthSpin_->setRange(kMinDefaultDialogW, 8000);
    defaultDialogWidthSpin_->setSuffix(QStringLiteral(" px"));
    defaultDialogWidthSpin_->setSingleStep(50);
    defaultDialogWidthSpin_->setFixedWidth(kPxSpinWidth);
    defaultDialogWidthSpin_->setCorrectionMode(
        QAbstractSpinBox::CorrectToNearestValue);
    dlgForm->addRow(tr("Default width:"), defaultDialogWidthSpin_);

    defaultDialogHeightSpin_ = new QSpinBox(dlgGroup);
    defaultDialogHeightSpin_->setRange(kMinDefaultDialogH, 5000);
    defaultDialogHeightSpin_->setSuffix(QStringLiteral(" px"));
    defaultDialogHeightSpin_->setSingleStep(50);
    defaultDialogHeightSpin_->setFixedWidth(kPxSpinWidth);
    defaultDialogHeightSpin_->setCorrectionMode(
        QAbstractSpinBox::CorrectToNearestValue);
    dlgForm->addRow(tr("Default height:"), defaultDialogHeightSpin_);

    // Live readout of THIS dialog's current size, mirroring the
    // "Current window size" line in the Default Window Size group
    // above. Refreshed in loadSettings() and apply() — useful for
    // picking values empirically (drag the dialog, click Apply,
    // read off the size).
    currentDialogSizeLabel_ = new QLabel(dlgGroup);
    currentDialogSizeLabel_->setStyleSheet(
        QStringLiteral("QLabel { color: palette(mid); }"));
    dlgForm->addRow(QString(), currentDialogSizeLabel_);

    auto syncDlgSizeEnabled = [this]() {
        const bool restore = restoreLastDialogSizeCheck_
            && restoreLastDialogSizeCheck_->isChecked();
        if (defaultDialogWidthSpin_)
            defaultDialogWidthSpin_->setEnabled(!restore);
        if (defaultDialogHeightSpin_)
            defaultDialogHeightSpin_->setEnabled(!restore);
    };
    connect(restoreLastDialogSizeCheck_, &QCheckBox::toggled,
            this, syncDlgSizeEnabled);

    auto* dlgNote = new QLabel(
        tr("This size is applied to every popup dialog (Commit, "
           "Clone, Tag, Stash, Rebase, etc.) on next open. "
           "Dialogs whose layout requires more space than this "
           "will auto-grow to fit. Minimum %1 × %2 px — values "
           "below that snap to the minimum.")
            .arg(kMinDefaultDialogW).arg(kMinDefaultDialogH),
        dlgGroup);
    dlgNote->setWordWrap(true);
    dlgNote->setStyleSheet(
        QStringLiteral("QLabel { color: palette(mid); }"));
    dlgForm->addRow(QString(), dlgNote);

    root->addWidget(dlgGroup);

    if (settings_) {
        restoreLastDialogSizeCheck_->setChecked(
            settings_->restoreLastDialogSize());
        const QSize sz = settings_->defaultDialogSize();
        defaultDialogWidthSpin_ ->setValue(sz.width());
        defaultDialogHeightSpin_->setValue(sz.height());
    }
    syncDlgSizeEnabled();

    root->addStretch();

    return page;
}

// ---------------------------------------------------------------------------
// Git config page
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createGitConfigPage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    // [320, 360] px comfortably fits typical names, emails, and
    // remote names without sprawling across the dialog when it's
    // been dragged wide. The min keeps the box usable (a long
    // GitHub no-reply address fits without scrolling); the max
    // stops it from following the dialog out to 1000+ px.
    constexpr int kEditMinWidth = 320;
    constexpr int kEditMaxWidth = 360;
    auto sizeEdit = [](QLineEdit* e) {
        e->setMinimumWidth(kEditMinWidth);
        e->setMaximumWidth(kEditMaxWidth);
    };

    auto* identityGroup = new QGroupBox(tr("Identity"), page);
    auto* identityForm = new QFormLayout(identityGroup);
    identityForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    userNameEdit_ = new QLineEdit(identityGroup);
    userNameEdit_->setObjectName(QStringLiteral("settings.git-user-name"));
    userNameEdit_->setPlaceholderText(tr("Your Name"));
    sizeEdit(userNameEdit_);
    identityForm->addRow(tr("user.name:"), userNameEdit_);
    userEmailEdit_ = new QLineEdit(identityGroup);
    userEmailEdit_->setObjectName(QStringLiteral("settings.git-user-email"));
    userEmailEdit_->setPlaceholderText(tr("you@example.com"));
    sizeEdit(userEmailEdit_);
    identityForm->addRow(tr("user.email:"), userEmailEdit_);
    form->addRow(identityGroup);

    auto* remoteGroup = new QGroupBox(tr("Defaults"), page);
    auto* remoteForm = new QFormLayout(remoteGroup);
    remoteForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    defaultRemoteEdit_ = new QLineEdit(remoteGroup);
    defaultRemoteEdit_->setPlaceholderText(QStringLiteral("origin"));
    sizeEdit(defaultRemoteEdit_);
    remoteForm->addRow(tr("Default Remote:"), defaultRemoteEdit_);
    form->addRow(remoteGroup);

    auto* credGroup = new QGroupBox(tr("Credentials"), page);
    auto* credForm = new QFormLayout(credGroup);
    credForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    credentialHelperEdit_ = new QLineEdit(credGroup);
    credentialHelperEdit_->setReadOnly(true);
    credentialHelperEdit_->setPlaceholderText(tr("(not configured)"));
    sizeEdit(credentialHelperEdit_);
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
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    themeCombo_ = new QComboBox(page);
    themeCombo_->setObjectName(QStringLiteral("settings.theme"));
    themeCombo_->setMinimumWidth(180);
    themeCombo_->setMaximumWidth(260);
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
    shortcutsTable_->setObjectName(QStringLiteral("settings.shortcuts"));
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
// Plugins page — describes the built-in extensions and where their
// settings live. There is no dynamic plugin host — the previously
// scaffolded one (src/plugins/) was never instantiated and has been
// removed; the built-in features below are wired directly:
// every extension currently ships built into the Plugins menu, so
// this page documents that honestly instead of pretending there is
// a loadable-plugin list to manage.
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
        tr("GitBolt ships its extensions built in — find them under "
           "the <b>Plugins</b> menu:"),
        page);
    body->setTextFormat(Qt::RichText);
    body->setWordWrap(true);
    layout->addWidget(body);

    auto* list = new QLabel(
        tr("<ul>"
           "<li><b>Periodic background fetch</b> — toggleable, with an "
           "editable interval; persists across launches</li>"
           "<li><b>Repository statistics</b> — commit counts, top "
           "contributors, branch / tag / remote totals</li>"
           "<li><b>Git Flow</b> — init plus feature / release / hotfix "
           "start &amp; finish</li>"
           "<li><b>Delete obsolete branches</b> — multi-select picker "
           "over merged branches</li>"
           "<li><b>Find large files</b> — top blobs by size across "
           "all history</li>"
           "</ul>"),
        page);
    list->setTextFormat(Qt::RichText);
    list->setWordWrap(true);
    layout->addWidget(list);

    auto* footer = new QLabel(
        tr("Loading third-party plugins from shared libraries is on "
           "the roadmap; when it lands, installed plugins and their "
           "settings will be managed from this page."),
        page);
    footer->setWordWrap(true);
    footer->setStyleSheet(QStringLiteral("QLabel { color: palette(mid); }"));
    layout->addWidget(footer);

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
    // Put the FACTORY defaults into the table — the old version
    // re-ran populate, which read back the *saved* values: a no-op
    // precisely when there was something to reset. Takes effect on
    // Apply like any other pending edit.
    for (int i = 0; i < shortcutsTable_->rowCount(); ++i) {
        auto* nameItem = shortcutsTable_->item(i, 0);
        auto* keyItem  = shortcutsTable_->item(i, 1);
        if (!nameItem || !keyItem)
            continue;
        const QString name = nameItem->data(Qt::UserRole).toString();
        keyItem->setText(shortcutDefaults_.value(name)
                             .toString(QKeySequence::NativeText));
    }
}

void SettingsDialog::setShortcutActions(
    const QList<QAction*>& actions,
    const QHash<QString, QKeySequence>& defaults)
{
    shortcutActions_.clear();
    for (QAction* a : actions)
        shortcutActions_.append(QPointer<QAction>(a));
    shortcutDefaults_ = defaults;
    populateShortcutsTable();
}

void SettingsDialog::populateShortcutsTable()
{
    // Rows mirror the REAL QActions handed over by MainWindow. The
    // page used to list hand-typed "representative defaults" that
    // matched neither the actual bindings nor anything any code read
    // back — write-only settings under names no action carried.
    rowActions_.clear();
    shortcutsTable_->setRowCount(0);
    if (shortcutActions_.isEmpty())
        return;

    shortcutsTable_->setRowCount(shortcutActions_.size());
    int row = 0;
    for (const auto& ap : shortcutActions_) {
        QAction* a = ap.data();
        if (!a)
            continue;
        QString label = a->text();
        label.remove(QLatin1Char('&'));
        while (label.endsWith(QLatin1Char('.')) ||
               label.endsWith(QChar(0x2026)))  // trailing "..." / "…"
            label.chop(1);

        auto* nameItem = new QTableWidgetItem(label.trimmed());
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        nameItem->setData(Qt::UserRole, a->objectName());
        nameItem->setToolTip(a->objectName());
        shortcutsTable_->setItem(row, 0, nameItem);

        auto* keyItem = new QTableWidgetItem(
            a->shortcut().toString(QKeySequence::NativeText));
        keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
        shortcutsTable_->setItem(row, 1, keyItem);

        rowActions_.append(ap);
        ++row;
    }
    shortcutsTable_->setRowCount(row);
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

    // UI Design — startup window size
    if (restoreLastSizeCheck_)
        restoreLastSizeCheck_->setChecked(settings_->restoreLastWindowSize());
    if (startupWidthSpin_)
        startupWidthSpin_->setValue(settings_->startupWindowWidth());
    if (startupHeightSpin_)
        startupHeightSpin_->setValue(settings_->startupWindowHeight());

    // UI Design — single global default dialog size + toggle
    if (restoreLastDialogSizeCheck_)
        restoreLastDialogSizeCheck_->setChecked(
            settings_->restoreLastDialogSize());
    if (defaultDialogWidthSpin_ && defaultDialogHeightSpin_) {
        const QSize sz = settings_->defaultDialogSize();
        defaultDialogWidthSpin_ ->setValue(sz.width());
        defaultDialogHeightSpin_->setValue(sz.height());
    }
    if (currentDialogSizeLabel_) {
        const QSize sz = size();
        currentDialogSizeLabel_->setText(
            tr("Current dialog size: %1 × %2 px")
                .arg(sz.width()).arg(sz.height()));
    }

    // Refresh the live "Current window size" readout. Done here
    // (not just at construction) so reopening the Settings dialog
    // after the user dragged the main window picks up the new size
    // without a relaunch. The label format is intentionally simple
    // text so screen-reader users can also read it cleanly.
    if (currentMainWindowSize_) {
        if (auto* p = parentWidget()) {
            QWidget* top = p->window();
            if (top) {
                const QSize sz = top->size();
                currentMainWindowSize_->setText(
                    tr("Current window size: %1 × %2 px")
                        .arg(sz.width()).arg(sz.height()));
            }
        } else {
            currentMainWindowSize_->setText(QString{});
        }
    }

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
    // Read global git config via the git CLI for portability — on a
    // worker. The three sequential reads each carried a 3 s timeout
    // and ran in the constructor, so a slow git stalled the dialog
    // up to ~9 s before it could paint.
    userNameEdit_->setPlaceholderText(tr("loading…"));
    userEmailEdit_->setPlaceholderText(tr("loading…"));
    credentialHelperEdit_->setPlaceholderText(tr("loading…"));

    struct GitIdentity {
        QString name;
        QString email;
        QString credentialHelper;
    };
    auto* watcher = new QFutureWatcher<GitIdentity>(this);
    connect(watcher, &QFutureWatcher<GitIdentity>::finished, this,
            [this, watcher]() {
        const GitIdentity id = watcher->result();
        userNameEdit_->setText(id.name);
        userEmailEdit_->setText(id.email);
        credentialHelperEdit_->setText(id.credentialHelper);
        userNameEdit_->setPlaceholderText(QString());
        userEmailEdit_->setPlaceholderText(QString());
        credentialHelperEdit_->setPlaceholderText(QString());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([]() {
        auto readConfig = [](const QString& key) -> QString {
            QProcess proc;
            proc.start(QStringLiteral("git"),
                       {QStringLiteral("config"),
                        QStringLiteral("--global"), key});
            proc.waitForFinished(3000);
            return QString::fromUtf8(
                proc.readAllStandardOutput()).trimmed();
        };
        GitIdentity id;
        id.name  = readConfig(QStringLiteral("user.name"));
        id.email = readConfig(QStringLiteral("user.email"));
        id.credentialHelper =
            readConfig(QStringLiteral("credential.helper"));
        return id;
    }));
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

    // One settingsChanged for the whole Apply instead of one per
    // setter — consumers (RepositoryView, DashboardView, MainWindow)
    // re-read everything on each emission.
    settings_->beginUpdate();
    const auto endBatch = qScopeGuard([this] { settings_->endUpdate(); });

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

    // UI Design — startup window size. Persist the values, then
    // optionally resize the running main window so the user gets
    // immediate feedback. We only resize live when "Restore previous"
    // is OFF — when it's ON the spinboxes are disabled (the saved
    // geometry takes priority on next launch), so there's nothing
    // meaningful to apply to the current session.
    const bool restoreLast = restoreLastSizeCheck_
        && restoreLastSizeCheck_->isChecked();
    if (restoreLastSizeCheck_)
        settings_->setRestoreLastWindowSize(restoreLast);
    if (startupWidthSpin_)
        settings_->setStartupWindowWidth(startupWidthSpin_->value());
    if (startupHeightSpin_)
        settings_->setStartupWindowHeight(startupHeightSpin_->value());

    if (!restoreLast && startupWidthSpin_ && startupHeightSpin_) {
        // parentWidget() is the MainWindow; ->window() walks up to
        // the top-level widget if the dialog were ever reparented
        // under a child. Defensive against a null parent (which
        // shouldn't happen for a modal settings dialog, but the
        // dialog should still be usable in unit tests where it's
        // constructed standalone).
        if (QWidget* p = parentWidget()) {
            QWidget* top = p->window();
            if (top)
                top->resize(startupWidthSpin_->value(),
                            startupHeightSpin_->value());
        }
    }

    // Default dialog size — persist. Takes effect on the next
    // open of each dialog; we deliberately don't resize the
    // running Settings dialog (it would jump out from under the
    // user) or any open Commit dialog. The "Restore previous
    // dialog size" toggle controls whether dialogs prefer their
    // saved drag-resized geometry over the configured default
    // when opened.
    if (restoreLastDialogSizeCheck_)
        settings_->setRestoreLastDialogSize(
            restoreLastDialogSizeCheck_->isChecked());
    if (defaultDialogWidthSpin_ && defaultDialogHeightSpin_) {
        settings_->setDefaultDialogSize(
            QSize(defaultDialogWidthSpin_->value(),
                  defaultDialogHeightSpin_->value()));
    }
    // Refresh the "Current dialog size" label — if the user has
    // dragged the Settings dialog since opening it, the size is
    // likely different from what loadSettings() captured.
    if (currentDialogSizeLabel_) {
        const QSize sz = size();
        currentDialogSizeLabel_->setText(
            tr("Current dialog size: %1 × %2 px")
                .arg(sz.width()).arg(sz.height()));
    }

    // Refresh the "Current window size" label after Apply — if
    // the live resize above ran, the size is now different from
    // what the label showed at dialog open.
    if (currentMainWindowSize_) {
        if (QWidget* p = parentWidget()) {
            QWidget* top = p->window();
            if (top) {
                const QSize sz = top->size();
                currentMainWindowSize_->setText(
                    tr("Current window size: %1 × %2 px")
                        .arg(sz.width()).arg(sz.height()));
            }
        }
    }

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

    // Shortcuts — apply edits to the live QActions and persist only
    // real overrides; a binding put back to its factory default
    // clears the override key instead of pinning it forever.
    for (int i = 0; i < shortcutsTable_->rowCount()
                    && i < rowActions_.size(); ++i) {
        auto* nameItem = shortcutsTable_->item(i, 0);
        auto* keyItem  = shortcutsTable_->item(i, 1);
        QAction* action = rowActions_.at(i).data();
        if (!nameItem || !keyItem || !action)
            continue;
        const QString name = nameItem->data(Qt::UserRole).toString();
        if (name.isEmpty())
            continue;
        const QKeySequence seq = QKeySequence::fromString(
            keyItem->text(), QKeySequence::NativeText);
        if (action->shortcut() != seq)
            action->setShortcut(seq);
        const QString key = QStringLiteral("shortcuts/%1").arg(name);
        if (seq == shortcutDefaults_.value(name))
            settings_->remove(key);
        else
            settings_->setValue(
                key, seq.toString(QKeySequence::PortableText));
    }
}

void SettingsDialog::onAccepted()
{
    apply();
    accept();
}

} // namespace gitbolt::dialogs
