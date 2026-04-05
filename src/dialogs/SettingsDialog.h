#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFontComboBox;
class QKeySequenceEdit;
class QLineEdit;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QWidget;

namespace gitbolt::conf {
class SettingsService;
class ThemeService;
} // namespace gitbolt::conf

namespace gitbolt::dialogs {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(conf::SettingsService* settings,
                            conf::ThemeService* theme,
                            QWidget* parent = nullptr);

private slots:
    void apply();
    void onAccepted();
    void onShortcutCellDoubleClicked(int row, int column);
    void resetShortcutsToDefault();

private:
    QWidget* createGeneralTab();
    QWidget* createGitConfigTab();
    QWidget* createAppearanceTab();
    QWidget* createShortcutsTab();

    void loadSettings();
    void loadGitConfig();
    void saveGitConfig();
    void populateShortcutsTable();

    conf::SettingsService* settings_;
    conf::ThemeService* theme_;

    QTabWidget* tabs_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;

    // General tab
    QFontComboBox* fontCombo_ = nullptr;
    QSpinBox* fontSizeSpin_ = nullptr;
    QSpinBox* tabSizeSpin_ = nullptr;
    QCheckBox* showWhitespaceCheck_ = nullptr;

    // Git config tab
    QLineEdit* userNameEdit_ = nullptr;
    QLineEdit* userEmailEdit_ = nullptr;
    QLineEdit* defaultRemoteEdit_ = nullptr;
    QLineEdit* credentialHelperEdit_ = nullptr;

    // Appearance tab
    QComboBox* themeCombo_ = nullptr;
    QWidget* previewArea_ = nullptr;

    // Shortcuts tab
    QTableWidget* shortcutsTable_ = nullptr;
    QKeySequenceEdit* keySeqEdit_ = nullptr;
};

} // namespace gitbolt::dialogs
