#pragma once

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFontComboBox;
class QKeySequenceEdit;
class QLineEdit;
class QSpinBox;
class QSlider;
class QStackedWidget;
class QAction;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;
class QLabel;
class QListWidget;
class QRadioButton;

namespace gitbolt::conf {
class SettingsService;
class ThemeService;
} // namespace gitbolt::conf

namespace gitbolt::dialogs {

/// Modal preferences dialog styled after the GitExtensions settings
/// window: a QTreeWidget on the left lists three top-level categories
/// (GitBolt / Git / Plugins) with a flat list of sub-sections under
/// each, and a QStackedWidget on the right shows the page matching
/// the selected sub-section. Sub-sub-sections are intentionally not
/// supported — the tree is exactly two levels deep.
///
/// Pages live inside the dialog as plain QWidget instances, added
/// to the stack in the same order as the tree so selecting tree
/// row N shows stack index N. The mapping is bound by stashing the
/// stack index in each tree item's Qt::UserRole.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /// Feed the Shortcuts page the application's REAL actions and
    /// their factory-default bindings. Called by MainWindow before
    /// exec(); without it the page is empty.
    void setShortcutActions(const QList<QAction*>& actions,
                            const QHash<QString, QKeySequence>& defaults);

    explicit SettingsDialog(conf::SettingsService* settings,
                            conf::ThemeService* theme,
                            QWidget* parent = nullptr);

private slots:
    void apply();
    void onAccepted();
    void onTreeSelectionChanged();
    void onShortcutCellDoubleClicked(int row, int column);
    void resetShortcutsToDefault();

private:
    // Page builders — each returns a standalone QWidget that owns
    // its own layout. All controls are field members on the dialog
    // so loadSettings/apply() can read/write them.
    QWidget* createGeneralPage();
    QWidget* createUiDesignPage();
    QWidget* createAppearancePage();
    QWidget* createShortcutsPage();
    QWidget* createRecentReposPage();
    QWidget* createGitConfigPage();
    QWidget* createPluginsPage();

    // Add a sub-section under the given category parent, wire its
    // page into the stacked widget, and stash the stack index on
    // the tree item. Returns the added QTreeWidgetItem so callers
    // can select it as the default.
    QTreeWidgetItem* addSubsection(QTreeWidgetItem* parent,
                                   const QString& label,
                                   QWidget* page);

    void loadSettings();
    void loadGitConfig();
    void saveGitConfig();
    void populateShortcutsTable();

    conf::SettingsService* settings_;
    conf::ThemeService* theme_;

    // Left navigation + right pages
    QTreeWidget*     nav_   = nullptr;
    QStackedWidget*  pages_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;

    // General page
    QFontComboBox* fontCombo_ = nullptr;
    QSpinBox* fontSizeSpin_ = nullptr;
    QSpinBox* tabSizeSpin_ = nullptr;
    QCheckBox* showWhitespaceCheck_ = nullptr;

    // UI Design page — the default vertical split between the
    // revision graph (top) and the inspector tabs (bottom). The
    // slider and spinbox are two-way bound so dragging one updates
    // the other, and the label shows the live percent string.
    QSlider*  bottomPaneSlider_ = nullptr;
    QSpinBox* bottomPaneSpin_   = nullptr;
    QLabel*   bottomPanePreview_ = nullptr;

    // UI Design page — startup window size. The check controls
    // whether geometry is remembered across launches; when off,
    // the spin boxes' width × height are applied verbatim each
    // launch. When on, the spin boxes are disabled (the saved
    // geometry takes priority).
    QCheckBox* restoreLastSizeCheck_ = nullptr;
    QSpinBox*  startupWidthSpin_     = nullptr;
    QSpinBox*  startupHeightSpin_    = nullptr;

    // UI Design page — current size of the parent (main) window,
    // shown in muted text under the startup-size group so users
    // can read off the size they want before typing it into the
    // spinboxes. Refreshed on dialog open and after Apply.
    QLabel* currentMainWindowSize_ = nullptr;

    // UI Design page — single global default size that every
    // popup dialog (Settings, Commit, Clone, Tag, Stash, Rebase,
    // Reflog, Cherry-Pick, Worktree, Remotes, Text Editor,
    // Stash Manager) reads in its constructor. The toggle works
    // exactly like the one in the Default Window Size group: ON
    // restores each dialog's last drag-resized size on next open
    // and disables the spinboxes; OFF always uses the configured
    // default. The current-size label is a live readout of THIS
    // dialog's size — useful for picking values empirically.
    QCheckBox* restoreLastDialogSizeCheck_ = nullptr;
    QSpinBox*  defaultDialogWidthSpin_     = nullptr;
    QSpinBox*  defaultDialogHeightSpin_    = nullptr;
    QLabel*    currentDialogSizeLabel_     = nullptr;

    // Git config page
    QLineEdit* userNameEdit_ = nullptr;
    QLineEdit* userEmailEdit_ = nullptr;
    QLineEdit* defaultRemoteEdit_ = nullptr;
    QLineEdit* credentialHelperEdit_ = nullptr;

    // Appearance page
    QComboBox* themeCombo_ = nullptr;
    QWidget* previewArea_ = nullptr;

    // Shortcuts page
    QTableWidget* shortcutsTable_ = nullptr;
    QKeySequenceEdit* keySeqEdit_ = nullptr;

    // Real actions behind the Shortcuts page (handed over by
    // MainWindow::showSettingsDialog). rowActions_ maps table rows
    // back to actions across repopulations; defaults back the
    // "Reset to Default" button and let Apply drop overrides that
    // match the factory binding.
    QList<QPointer<QAction>> shortcutActions_;
    QList<QPointer<QAction>> rowActions_;
    QHash<QString, QKeySequence> shortcutDefaults_;

    // Recent Repositories page — mirrors GitExtensions' dialog:
    // a max-count spinbox, an alphabetical-sort toggle, three
    // radio buttons for the path-shortening strategy, and a live
    // list of the current recents (read-only; Clear happens from
    // the dashboard).
    QSpinBox*     recentMaxCountSpin_   = nullptr;
    QCheckBox*    recentSortCheck_      = nullptr;
    QRadioButton* recentShortenNone_    = nullptr;
    QRadioButton* recentShortenMiddle_  = nullptr;
    QRadioButton* recentShortenSigDir_  = nullptr;
    QListWidget*  recentListPreview_    = nullptr;
};

} // namespace gitbolt::dialogs
