#include "conf/SettingsService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace gitbolt::conf {

SettingsService::SettingsService(QObject* parent)
    : QObject(parent)
    , settings_(QSettings::IniFormat, QSettings::UserScope,
                QCoreApplication::organizationName().isEmpty()
                    ? QStringLiteral("GitBolt")
                    : QCoreApplication::organizationName(),
                QCoreApplication::applicationName().isEmpty()
                    ? QStringLiteral("GitBolt")
                    : QCoreApplication::applicationName())
{
}

// ---------------------------------------------------------------------------
// Recent repositories
// ---------------------------------------------------------------------------

QStringList SettingsService::recentRepositories() const
{
    return settings_.value(QStringLiteral("recent/repositories")).toStringList();
}

void SettingsService::addRecentRepository(const QString& path)
{
    QString canonical = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    QStringList list = recentRepositories();
    list.removeAll(canonical);
    list.prepend(canonical);
    while (list.size() > kMaxRecentRepositories)
        list.removeLast();
    settings_.setValue(QStringLiteral("recent/repositories"), list);
    emit settingsChanged();
}

void SettingsService::removeRecentRepository(const QString& path)
{
    QString canonical = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    QStringList list = recentRepositories();
    if (list.removeAll(canonical) > 0) {
        settings_.setValue(QStringLiteral("recent/repositories"), list);
        emit settingsChanged();
    }
}

void SettingsService::clearRecentRepositories()
{
    settings_.remove(QStringLiteral("recent/repositories"));
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
// Code font
// ---------------------------------------------------------------------------

QFont SettingsService::codeFont() const
{
    QFont defaultFont(QStringLiteral("Menlo, Consolas, Courier New"));
    defaultFont.setPointSize(codeFontSize());
    QFont font;
    font.fromString(settings_.value(QStringLiteral("editor/font"), defaultFont.toString()).toString());
    return font;
}

void SettingsService::setCodeFont(const QFont& font)
{
    settings_.setValue(QStringLiteral("editor/font"), font.toString());
    emit settingsChanged();
}

int SettingsService::codeFontSize() const
{
    return settings_.value(QStringLiteral("editor/fontSize"), 12).toInt();
}

void SettingsService::setCodeFontSize(int size)
{
    settings_.setValue(QStringLiteral("editor/fontSize"), qBound(6, size, 72));
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
// Git defaults
// ---------------------------------------------------------------------------

QString SettingsService::defaultRemote() const
{
    return settings_.value(QStringLiteral("git/defaultRemote"), QStringLiteral("origin")).toString();
}

void SettingsService::setDefaultRemote(const QString& remote)
{
    settings_.setValue(QStringLiteral("git/defaultRemote"), remote);
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

int SettingsService::tabSize() const
{
    return settings_.value(QStringLiteral("editor/tabSize"), 4).toInt();
}

void SettingsService::setTabSize(int size)
{
    settings_.setValue(QStringLiteral("editor/tabSize"), qBound(1, size, 16));
    emit settingsChanged();
}

bool SettingsService::showWhitespace() const
{
    return settings_.value(QStringLiteral("editor/showWhitespace"), false).toBool();
}

void SettingsService::setShowWhitespace(bool show)
{
    settings_.setValue(QStringLiteral("editor/showWhitespace"), show);
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------

void SettingsService::saveWindowGeometry(const QByteArray& geometry)
{
    settings_.setValue(QStringLiteral("window/geometry"), geometry);
}

QByteArray SettingsService::restoreWindowGeometry() const
{
    return settings_.value(QStringLiteral("window/geometry")).toByteArray();
}

void SettingsService::saveWindowState(const QByteArray& state)
{
    settings_.setValue(QStringLiteral("window/state"), state);
}

QByteArray SettingsService::restoreWindowState() const
{
    return settings_.value(QStringLiteral("window/state")).toByteArray();
}

// ---------------------------------------------------------------------------
// Raw access
// ---------------------------------------------------------------------------

QVariant SettingsService::value(const QString& key, const QVariant& defaultValue) const
{
    return settings_.value(key, defaultValue);
}

void SettingsService::setValue(const QString& key, const QVariant& val)
{
    settings_.setValue(key, val);
    emit settingsChanged();
}

} // namespace gitbolt::conf
