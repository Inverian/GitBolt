#pragma once

#include <QByteArray>
#include <QFont>
#include <QObject>
#include <QSettings>
#include <QStringList>

namespace gitbolt::conf {

class SettingsService : public QObject {
    Q_OBJECT
public:
    explicit SettingsService(QObject* parent = nullptr);

    // Recent repositories (max 10)
    QStringList recentRepositories() const;
    void addRecentRepository(const QString& path);
    void removeRecentRepository(const QString& path);
    void clearRecentRepositories();

    // UI — code font
    QFont codeFont() const;
    void setCodeFont(const QFont& font);
    int codeFontSize() const;
    void setCodeFontSize(int size);

    // Git defaults
    QString defaultRemote() const;
    void setDefaultRemote(const QString& remote);

    // Behaviour
    int tabSize() const;
    void setTabSize(int size);
    bool showWhitespace() const;
    void setShowWhitespace(bool show);

    // Window state persistence
    void saveWindowGeometry(const QByteArray& geometry);
    QByteArray restoreWindowGeometry() const;
    void saveWindowState(const QByteArray& state);
    QByteArray restoreWindowState() const;

    // Raw access for ad-hoc keys
    QVariant value(const QString& key, const QVariant& defaultValue = {}) const;
    void setValue(const QString& key, const QVariant& value);

signals:
    void settingsChanged();

private:
    static constexpr int kMaxRecentRepositories = 10;
    QSettings settings_;
};

} // namespace gitbolt::conf
