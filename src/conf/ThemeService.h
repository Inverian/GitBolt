#pragma once

#include <QColor>
#include <QObject>
#include <QStringList>

#include <functional>

class QApplication;
class QWidget;

namespace gitbolt::conf {

class ThemeService : public QObject {
    Q_OBJECT
public:
    explicit ThemeService(QObject* parent = nullptr);

    /// Available theme identifiers: "Light", "Dark", "System".
    QStringList availableThemes() const;

    /// Current theme name.
    QString currentTheme() const;

    /// Set the active theme and apply it immediately.
    void setTheme(const QString& name);

    /// Apply the current theme to the given QApplication.
    void applyTheme(QApplication* app);

    /// Returns true when the effective palette is dark.
    bool isDarkTheme() const;

    /// Whether the application palette in effect is a dark one (its
    /// window text lighter than its window), whichever theme set it.
    static bool paletteIsDark();

    /// Secondary text (a note under a setting, a hint, a placeholder),
    /// in the theme in effect: #9a9a9a on the dark theme (6.0:1 on its
    /// window, 4.9:1 on its tool bars), #6a6a6a on the light one (4.7:1
    /// on its window). palette(mid), which such text used, is a
    /// border's grey: 2.1:1 and 1.7:1.
    static QColor mutedTextColor();

    /// Sets `widget`'s style sheet to what `sheet` gives for the theme
    /// in effect (dark or not), and again on every theme switch (the
    /// PaletteChange it gets): a colour picked once kept the theme of
    /// the moment.
    static void keepThemedStyleSheet(QWidget* widget,
                                     std::function<QString(bool dark)> sheet);
    /// keepThemedStyleSheet() with `sheet`'s "%1" the muted text colour.
    static void keepMutedText(QWidget* widget, const QString& sheet);

signals:
    void themeChanged(const QString& name);

private:
    void applyDarkPalette(QApplication* app);
    void applyLightPalette(QApplication* app);
    QString darkStyleSheet() const;
    QString lightStyleSheet() const;

    QString currentTheme_ = QStringLiteral("System");
};

} // namespace gitbolt::conf
