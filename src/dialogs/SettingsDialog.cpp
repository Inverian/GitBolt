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
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

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
    resize(640, 480);

    auto* layout = new QVBoxLayout(this);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(createGeneralTab(), tr("General"));
    tabs_->addTab(createGitConfigTab(), tr("Git Config"));
    tabs_->addTab(createAppearanceTab(), tr("Appearance"));
    tabs_->addTab(createShortcutsTab(), tr("Shortcuts"));
    layout->addWidget(tabs_);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    layout->addWidget(buttons_);

    connect(buttons_, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::apply);

    loadSettings();
    loadGitConfig();
}

// ---------------------------------------------------------------------------
// General tab
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createGeneralTab()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    auto* fontRow = new QHBoxLayout;
    fontCombo_ = new QFontComboBox(this);
    fontCombo_->setFontFilters(QFontComboBox::MonospacedFonts);
    fontSizeSpin_ = new QSpinBox(this);
    fontSizeSpin_->setRange(6, 72);
    fontRow->addWidget(fontCombo_, 1);
    fontRow->addWidget(fontSizeSpin_);
    form->addRow(tr("Code Font:"), fontRow);

    tabSizeSpin_ = new QSpinBox(this);
    tabSizeSpin_->setRange(1, 16);
    form->addRow(tr("Tab Size:"), tabSizeSpin_);

    showWhitespaceCheck_ = new QCheckBox(tr("Show whitespace characters"), this);
    form->addRow(QString(), showWhitespaceCheck_);

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return page;
}

// ---------------------------------------------------------------------------
// Git Config tab
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createGitConfigTab()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    auto* identityGroup = new QGroupBox(tr("Identity"), this);
    auto* identityForm = new QFormLayout(identityGroup);
    userNameEdit_ = new QLineEdit(this);
    userNameEdit_->setPlaceholderText(tr("Your Name"));
    identityForm->addRow(tr("user.name:"), userNameEdit_);
    userEmailEdit_ = new QLineEdit(this);
    userEmailEdit_->setPlaceholderText(tr("you@example.com"));
    identityForm->addRow(tr("user.email:"), userEmailEdit_);
    form->addRow(identityGroup);

    auto* remoteGroup = new QGroupBox(tr("Defaults"), this);
    auto* remoteForm = new QFormLayout(remoteGroup);
    defaultRemoteEdit_ = new QLineEdit(this);
    defaultRemoteEdit_->setPlaceholderText(QStringLiteral("origin"));
    remoteForm->addRow(tr("Default Remote:"), defaultRemoteEdit_);
    form->addRow(remoteGroup);

    auto* credGroup = new QGroupBox(tr("Credentials"), this);
    auto* credForm = new QFormLayout(credGroup);
    credentialHelperEdit_ = new QLineEdit(this);
    credentialHelperEdit_->setReadOnly(true);
    credentialHelperEdit_->setPlaceholderText(tr("(not configured)"));
    credForm->addRow(tr("Credential Helper:"), credentialHelperEdit_);
    form->addRow(credGroup);

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return page;
}

// ---------------------------------------------------------------------------
// Appearance tab
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createAppearanceTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* form = new QFormLayout;
    themeCombo_ = new QComboBox(this);
    if (theme_)
        themeCombo_->addItems(theme_->availableThemes());
    form->addRow(tr("Theme:"), themeCombo_);
    layout->addLayout(form);

    // Preview area showing sampled palette colors
    auto* previewGroup = new QGroupBox(tr("Theme Preview"), this);
    auto* previewLayout = new QVBoxLayout(previewGroup);

    previewArea_ = new QWidget(this);
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
        auto* frame = new QFrame(this);
        frame->setFixedSize(60, 30);
        frame->setAutoFillBackground(true);
        QPalette fp;
        fp.setColor(QPalette::Window, QApplication::palette().color(role));
        frame->setPalette(fp);
        frame->setFrameStyle(QFrame::Box);
        auto* col = new QVBoxLayout;
        col->setSpacing(2);
        col->addWidget(frame, 0, Qt::AlignCenter);
        col->addWidget(new QLabel(label, this), 0, Qt::AlignCenter);
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
// Shortcuts tab
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::createShortcutsTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    shortcutsTable_ = new QTableWidget(this);
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
    keySeqEdit_ = new QKeySequenceEdit(this);
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
    auto* resetBtn = new QPushButton(tr("Reset to Default"), this);
    connect(resetBtn, &QPushButton::clicked, this, &SettingsDialog::resetShortcutsToDefault);
    btnRow->addWidget(resetBtn);
    layout->addLayout(btnRow);

    populateShortcutsTable();
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

    if (theme_) {
        int idx = themeCombo_->findText(theme_->currentTheme());
        if (idx >= 0)
            themeCombo_->setCurrentIndex(idx);
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

    // Git
    settings_->setDefaultRemote(defaultRemoteEdit_->text().trimmed());
    saveGitConfig();

    // Theme
    if (theme_)
        theme_->setTheme(themeCombo_->currentText());

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
