#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFontComboBox;
class QKeySequenceEdit;
class QLineEdit;
class QSpinBox;
class QSlider;
class QStackedWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;
class QLabel;

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
};

} // namespace gitbolt::dialogs
