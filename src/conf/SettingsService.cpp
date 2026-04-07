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
    // Filter out any empty strings that may have crept in — QSettings
    // can persist empty list entries if we ever wrote the key with an
    // empty value directly (which some external tools do for "clear"),
    // and an empty entry would render in the UI as a blank row with
    // no name. We strip them here on read rather than trying to catch
    // every write path.
    QStringList result = settings_.value(QStringLiteral("recent/repositories")).toStringList();
    result.removeAll(QString{});
    return result;
}

// Normalize a repository path to a canonical form for storage and
// comparison. Strips trailing slashes, resolves "." and ".." segments,
// uses native separators, and converts to an absolute path. Without
// this, "/Users/me/proj" and "/Users/me/proj/" are stored as two
// separate recent entries, and one of them renders with an empty
// display name (because QFileInfo::fileName() on a trailing-slash
// path returns the empty string).
static QString canonicalizeRepoPath(const QString& path)
{
    QString abs = QFileInfo(path).absoluteFilePath();
    // QDir::cleanPath strips redundant separators, resolves "." and
    // "..", and normalizes a trailing slash to a non-trailing one
    // (except for the root "/").
    abs = QDir::cleanPath(abs);
    return QDir::toNativeSeparators(abs);
}

void SettingsService::addRecentRepository(const QString& path)
{
    const QString canonical = canonicalizeRepoPath(path);
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
    const QString canonical = canonicalizeRepoPath(path);
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
