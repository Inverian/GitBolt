#include "conf/ThemeService.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>

namespace gitbolt::conf {

ThemeService::ThemeService(QObject* parent)
    : QObject(parent)
{
}

QStringList ThemeService::availableThemes() const
{
    return {
        QStringLiteral("Light"),
        QStringLiteral("Dark"),
        QStringLiteral("System"),
    };
}

QString ThemeService::currentTheme() const
{
    return currentTheme_;
}

void ThemeService::setTheme(const QString& name)
{
    if (currentTheme_ == name)
        return;
    currentTheme_ = name;
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
        applyTheme(app);
    emit themeChanged(name);
}

void ThemeService::applyTheme(QApplication* app)
{
    if (!app)
        return;

    bool useDark = false;

    if (currentTheme_ == QStringLiteral("Dark")) {
        useDark = true;
    } else if (currentTheme_ == QStringLiteral("System")) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        auto scheme = QGuiApplication::styleHints()->colorScheme();
        useDark = (scheme == Qt::ColorScheme::Dark);
#else
        // Heuristic: check if the default window text is lighter than the background.
        QPalette sys = QApplication::style()->standardPalette();
        useDark = sys.color(QPalette::WindowText).lightness() >
                  sys.color(QPalette::Window).lightness();
#endif
    }

    if (useDark)
        applyDarkPalette(app);
    else
        applyLightPalette(app);
}

bool ThemeService::isDarkTheme() const
{
    if (currentTheme_ == QStringLiteral("Dark"))
        return true;
    if (currentTheme_ == QStringLiteral("Light"))
        return false;

    // System detection
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    QPalette pal = QApplication::palette();
    return pal.color(QPalette::WindowText).lightness() >
           pal.color(QPalette::Window).lightness();
#endif
}

// ---------------------------------------------------------------------------
// Dark palette
// ---------------------------------------------------------------------------

void ThemeService::applyDarkPalette(QApplication* app)
{
    QPalette p;

    const QColor darkBg(30, 30, 30);
    const QColor darkAltBg(45, 45, 45);
    const QColor darkFg(212, 212, 212);
    const QColor disabledFg(128, 128, 128);
    const QColor highlight(0, 120, 212);
    const QColor highlightedText(255, 255, 255);
    const QColor link(86, 156, 214);
    const QColor tooltipBg(60, 60, 60);
    const QColor midColor(80, 80, 80);

    p.setColor(QPalette::Window, darkBg);
    p.setColor(QPalette::WindowText, darkFg);
    p.setColor(QPalette::Base, QColor(25, 25, 25));
    p.setColor(QPalette::AlternateBase, darkAltBg);
    p.setColor(QPalette::ToolTipBase, tooltipBg);
    p.setColor(QPalette::ToolTipText, darkFg);
    p.setColor(QPalette::PlaceholderText, disabledFg);
    p.setColor(QPalette::Text, darkFg);
    p.setColor(QPalette::Button, darkAltBg);
    p.setColor(QPalette::ButtonText, darkFg);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, highlightedText);
    p.setColor(QPalette::Mid, midColor);
    p.setColor(QPalette::Dark, QColor(18, 18, 18));
    p.setColor(QPalette::Shadow, QColor(10, 10, 10));
    p.setColor(QPalette::Light, QColor(60, 60, 60));
    p.setColor(QPalette::Midlight, QColor(50, 50, 50));

    // Disabled state
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledFg);
    p.setColor(QPalette::Disabled, QPalette::Text, disabledFg);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledFg);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledFg);

    app->setPalette(p);
    app->setStyleSheet(darkStyleSheet());
}

// ---------------------------------------------------------------------------
// Light palette
// ---------------------------------------------------------------------------

void ThemeService::applyLightPalette(QApplication* app)
{
    // Reset to default system palette
    app->setPalette(app->style()->standardPalette());
    app->setStyleSheet(lightStyleSheet());
}

// ---------------------------------------------------------------------------
// Stylesheets
// ---------------------------------------------------------------------------

QString ThemeService::darkStyleSheet() const
{
    return QStringLiteral(R"(
        QToolTip {
            color: #d4d4d4;
            background-color: #3c3c3c;
            border: 1px solid #555;
            padding: 4px;
        }
        QScrollBar:vertical {
            background: #1e1e1e;
            width: 12px;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: #5a5a5a;
            min-height: 20px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #787878;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar:horizontal {
            background: #1e1e1e;
            height: 12px;
            margin: 0;
        }
        QScrollBar::handle:horizontal {
            background: #5a5a5a;
            min-width: 20px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #787878;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
        }
        QTabWidget::pane {
            border: 1px solid #3c3c3c;
        }
        QTabBar::tab {
            background: #2d2d2d;
            color: #d4d4d4;
            padding: 6px 14px;
            border: 1px solid #3c3c3c;
            border-bottom: none;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: #1e1e1e;
            border-bottom: 2px solid #0078d4;
        }
        QTabBar::tab:hover:!selected {
            background: #383838;
        }
        QMenuBar {
            background: #2d2d2d;
            color: #d4d4d4;
        }
        QMenuBar::item:selected {
            background: #3c3c3c;
        }
        QMenu {
            background: #2d2d2d;
            color: #d4d4d4;
            border: 1px solid #3c3c3c;
        }
        QMenu::item:selected {
            background: #0078d4;
        }
        QStatusBar {
            background: #007acc;
            color: #ffffff;
        }
        QDockWidget::title {
            background: #2d2d2d;
            padding: 4px;
        }
        QHeaderView::section {
            background: #2d2d2d;
            color: #d4d4d4;
            padding: 4px;
            border: 1px solid #3c3c3c;
        }
        QTreeView, QListView, QTableView {
            alternate-background-color: #252525;
            selection-background-color: #0078d4;
        }
        QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QComboBox {
            background: #1e1e1e;
            color: #d4d4d4;
            border: 1px solid #3c3c3c;
            padding: 2px 4px;
            selection-background-color: #264f78;
        }
        QPushButton {
            background: #3c3c3c;
            color: #d4d4d4;
            border: 1px solid #555;
            padding: 4px 16px;
            border-radius: 2px;
        }
        QPushButton:hover {
            background: #505050;
        }
        QPushButton:pressed {
            background: #2d2d2d;
        }
        QPushButton:disabled {
            color: #808080;
        }
        QGroupBox {
            border: 1px solid #3c3c3c;
            margin-top: 8px;
            padding-top: 8px;
        }
        QGroupBox::title {
            color: #d4d4d4;
            subcontrol-origin: margin;
            left: 8px;
        }
    )");
}

QString ThemeService::lightStyleSheet() const
{
    // Minimal overrides — mostly rely on the default palette.
    return QStringLiteral(R"(
        QScrollBar:vertical {
            width: 12px;
        }
        QScrollBar::handle:vertical {
            min-height: 20px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar:horizontal {
            height: 12px;
        }
        QScrollBar::handle:horizontal {
            min-width: 20px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            width: 0;
            height: 0;
        }
    )");
}

} // namespace gitbolt::conf
