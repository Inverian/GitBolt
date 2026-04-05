#pragma once

#include <QObject>
#include <QStringList>

class QApplication;

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
