#pragma once
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QFont>

namespace gitbolt::conf {

class SettingsService : public QObject {
    Q_OBJECT
public:
    explicit SettingsService(QObject* parent = nullptr);
    QStringList recentRepositories() const;
    void addRecentRepository(const QString& path);
    void removeRecentRepository(const QString& path);
    QString theme() const;
    void setTheme(const QString& theme);
    QFont codeFont() const;
    void setCodeFont(const QFont& font);

private:
    QSettings settings_;
};

} // namespace gitbolt::conf
