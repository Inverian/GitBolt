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
    const int cap = maxRecentRepositories();
    while (list.size() > cap)
        list.removeLast();
    settings_.setValue(QStringLiteral("recent/repositories"), list);

    // Record the access timestamp alongside the path list. We persist
    // the map as a QVariantMap of ISO-8601 strings — QSettings writes
    // QVariantMap to its INI file natively, but QDateTime values inside
    // that map don't round-trip cleanly on every platform, so we
    // serialize to ISO strings explicitly. We also prune any entries
    // that aren't in the capped-at-10 recents list, to avoid unbounded
    // growth of the map for paths that have since rotated out.
    QVariantMap accessMap = settings_
        .value(QStringLiteral("recent/accessedAt")).toMap();
    accessMap[canonical] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    for (auto it = accessMap.begin(); it != accessMap.end();) {
        if (!list.contains(it.key()))
            it = accessMap.erase(it);
        else
            ++it;
    }
    settings_.setValue(QStringLiteral("recent/accessedAt"), accessMap);

    emit settingsChanged();
}

void SettingsService::removeRecentRepository(const QString& path)
{
    const QString canonical = canonicalizeRepoPath(path);
    QStringList list = recentRepositories();
    if (list.removeAll(canonical) > 0) {
        settings_.setValue(QStringLiteral("recent/repositories"), list);
        // Drop the access timestamp too — a repo that's been removed
        // from recents shouldn't leave its timestamp behind.
        QVariantMap accessMap = settings_
            .value(QStringLiteral("recent/accessedAt")).toMap();
        if (accessMap.remove(canonical) > 0)
            settings_.setValue(QStringLiteral("recent/accessedAt"), accessMap);
        emit settingsChanged();
    }
}

void SettingsService::clearRecentRepositories()
{
    settings_.remove(QStringLiteral("recent/repositories"));
    settings_.remove(QStringLiteral("recent/accessedAt"));
    emit settingsChanged();
}

QHash<QString, QDateTime> SettingsService::recentAccessTimes() const
{
    QHash<QString, QDateTime> result;
    const QVariantMap accessMap = settings_
        .value(QStringLiteral("recent/accessedAt")).toMap();
    for (auto it = accessMap.cbegin(); it != accessMap.cend(); ++it) {
        // Stored as ISO-8601 strings; parse back to QDateTime. If the
        // parse fails (corrupted value), skip the entry rather than
        // emitting an invalid QDateTime that callers would have to
        // defensively re-check.
        QDateTime dt = QDateTime::fromString(it.value().toString(), Qt::ISODate);
        if (dt.isValid())
            result.insert(it.key(), dt);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Recent repositories — display / retention preferences
// ---------------------------------------------------------------------------
//
// These live under the same "recent/" namespace as the list itself so
// a user wiping the whole recent-repos state (e.g. to migrate between
// machines) only has to clear a single prefix.

int SettingsService::maxRecentRepositories() const
{
    const int raw = settings_.value(QStringLiteral("recent/maxCount"),
                                    kDefaultMaxRecentRepositories).toInt();
    // Clamp here rather than only on set() — older builds may have
    // written an out-of-range value, and we want read() to be robust
    // against that without a silent migration.
    return qBound(1, raw, 200);
}

void SettingsService::setMaxRecentRepositories(int count)
{
    settings_.setValue(QStringLiteral("recent/maxCount"), qBound(1, count, 200));
    emit settingsChanged();
}

bool SettingsService::sortRecentAlphabetically() const
{
    return settings_.value(QStringLiteral("recent/sortAlphabetically"), false).toBool();
}

void SettingsService::setSortRecentAlphabetically(bool sort)
{
    settings_.setValue(QStringLiteral("recent/sortAlphabetically"), sort);
    emit settingsChanged();
}

SettingsService::RecentShortening SettingsService::recentShorteningStrategy() const
{
    const int raw = settings_.value(QStringLiteral("recent/shortening"),
                                    static_cast<int>(RecentShortening::None)).toInt();
    // Defensive cast — any unknown value rounds back to None so the
    // UI doesn't render as blank when the user has a future-schema
    // settings file from a newer build.
    if (raw == static_cast<int>(RecentShortening::MiddleEllipsis))
        return RecentShortening::MiddleEllipsis;
    if (raw == static_cast<int>(RecentShortening::SignificantDir))
        return RecentShortening::SignificantDir;
    return RecentShortening::None;
}

void SettingsService::setRecentShorteningStrategy(RecentShortening strategy)
{
    settings_.setValue(QStringLiteral("recent/shortening"),
                       static_cast<int>(strategy));
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
// UI Design — repo view bottom pane default size
// ---------------------------------------------------------------------------
//
// Stored as a 10..90 integer percent so it round-trips cleanly
// through QSettings and the settings dialog's spinbox. Applied to
// RepositoryView's inner vertical splitter by applyBottomPanePercent
// whenever the percent changes (via settingsChanged) and by the
// first-show restore path when there's no saved splitter state yet.

int SettingsService::bottomPanePercent() const
{
    // Default chosen empirically: 45% for the bottom inspector (commit
    // details / diff / file tree / GPG / console) leaves enough vertical
    // room for a meaningful diff preview on a 1280-tall window without
    // crowding the revision graph above. Bumped from 40 → 45 so the
    // inspector tabs don't feel cramped at first launch.
    return settings_.value(QStringLiteral("ui/bottomPanePercent"), 45).toInt();
}

void SettingsService::setBottomPanePercent(int percent)
{
    settings_.setValue(QStringLiteral("ui/bottomPanePercent"),
                       qBound(10, percent, 90));
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
// Layout persistence (splitters, dialog geometries)
// ---------------------------------------------------------------------------
//
// These are thin wrappers around QSettings that namespace everything
// under "layout/". They exist so the rest of the code doesn't have to
// spell out the prefix every time and so there's a single place to
// intercept reads/writes if we ever want to version-bump the whole
// layout schema.

static QString layoutKey(const QString& key)
{
    // Defensive: callers already pass keys like "repoSplitterH/v1",
    // but make sure there's no leading slash that would turn into
    // "layout//foo".
    QString k = key;
    while (k.startsWith(QLatin1Char('/')))
        k.remove(0, 1);
    return QStringLiteral("layout/") + k;
}

void SettingsService::saveSplitterState(const QString& key, const QByteArray& state)
{
    settings_.setValue(layoutKey(key), state);
}

QByteArray SettingsService::restoreSplitterState(const QString& key) const
{
    return settings_.value(layoutKey(key)).toByteArray();
}

void SettingsService::saveDialogGeometry(const QString& key, const QByteArray& geometry)
{
    settings_.setValue(layoutKey(key), geometry);
}

QByteArray SettingsService::restoreDialogGeometry(const QString& key) const
{
    return settings_.value(layoutKey(key)).toByteArray();
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
