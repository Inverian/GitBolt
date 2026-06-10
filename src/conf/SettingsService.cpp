#include "conf/SettingsService.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QShortcut>

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
// Startup window size
// ---------------------------------------------------------------------------
//
// When the user hasn't configured a value yet, default to ~80% of
// the primary screen's available geometry, clamped to a sane range
// so we don't pick something tiny on a phone-sized display or
// stretch silly-wide on a 5K ultrawide. The setter clamps to wider
// bounds — a typoed value should still be writable but not break
// the UI on the next launch.
//
// `availableGeometry()` excludes the macOS dock and menu bar, which
// is what we want — opening at full screenGeometry() would put the
// title bar under the menu bar.

namespace {

// Functional minimums — below these the sidebar, repo view, and
// inspector tabs start crowding to the point where the app stops
// being usable. Setters clamp to these via qBound, so a typoed
// smaller value (or one written by an older build) snaps up to the
// floor on read AND on write. The dialog's QSpinBox shares the same
// floor and auto-corrects any user input below it on focus-out.
constexpr int kStartupWidthMin  = 1024;
constexpr int kStartupWidthMax  = 8000;
constexpr int kStartupHeightMin = 700;
constexpr int kStartupHeightMax = 5000;

QSize defaultStartupSize()
{
    // Preferred new-install default. We still respect the available
    // screen size on small displays — a 1366 × 768 laptop would
    // overflow at 1400 × 900, so we cap to ~95% of the available
    // geometry as a safety net. Min floor is the functional minimum
    // (kStartupWidthMin / kStartupHeightMin) so we never propose
    // below the usable threshold.
    constexpr int kPreferredW = 1400;
    constexpr int kPreferredH = 900;
    QScreen* scr = QGuiApplication::primaryScreen();
    const QSize avail = scr ? scr->availableGeometry().size()
                            : QSize(1920, 1080);
    const int w = qMax(kStartupWidthMin,
                       qMin(kPreferredW, int(avail.width()  * 0.95)));
    const int h = qMax(kStartupHeightMin,
                       qMin(kPreferredH, int(avail.height() * 0.95)));
    return QSize(w, h);
}

} // namespace

int SettingsService::startupWindowWidth() const
{
    if (settings_.contains(QStringLiteral("window/startupWidth"))) {
        const int raw = settings_.value(
            QStringLiteral("window/startupWidth")).toInt();
        return qBound(kStartupWidthMin, raw, kStartupWidthMax);
    }
    return defaultStartupSize().width();
}

void SettingsService::setStartupWindowWidth(int width)
{
    settings_.setValue(QStringLiteral("window/startupWidth"),
                       qBound(kStartupWidthMin, width, kStartupWidthMax));
    emit settingsChanged();
}

int SettingsService::startupWindowHeight() const
{
    if (settings_.contains(QStringLiteral("window/startupHeight"))) {
        const int raw = settings_.value(
            QStringLiteral("window/startupHeight")).toInt();
        return qBound(kStartupHeightMin, raw, kStartupHeightMax);
    }
    return defaultStartupSize().height();
}

void SettingsService::setStartupWindowHeight(int height)
{
    settings_.setValue(QStringLiteral("window/startupHeight"),
                       qBound(kStartupHeightMin, height, kStartupHeightMax));
    emit settingsChanged();
}

bool SettingsService::restoreLastWindowSize() const
{
    return settings_.value(
        QStringLiteral("window/restoreLastSize"), true).toBool();
}

void SettingsService::setRestoreLastWindowSize(bool restore)
{
    settings_.setValue(QStringLiteral("window/restoreLastSize"), restore);
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
// Dialog default size
// ---------------------------------------------------------------------------
//
// One global W × H pair shared across every popup dialog the user
// can resize. Storage: dialog/defaultWidth, dialog/defaultHeight.
// Built-in default is 1000 × 800 — large enough that content-heavy
// dialogs (Commit, Rebase, Reflog) don't feel cramped on first
// open, while still leaving room on a typical 1400-wide window.
//
// Each dialog calls loadDefaultDialogSize() in its constructor
// and passes the result to resize(). Dialogs whose layout needs
// more vertical or horizontal space than this default auto-grow
// via Qt's minimum-size-hint propagation, so the user picking a
// small default doesn't break a content-heavy dialog like Commit
// or Rebase.
//
// Hard floor / ceiling protect against a typoed huge value
// breaking the UI; the settings dialog UI applies a tighter
// functional floor (kDefaultDialogWidthMin / Height) on top.
// The setter also drops the Commit dialog's persisted drag-
// resized geometry so a freshly-configured default isn't
// silently shadowed by a stale saved size.

namespace {

constexpr int kDefaultDialogWidthMin     = 400;
constexpr int kDefaultDialogWidthMax     = 8000;
constexpr int kDefaultDialogHeightMin    = 300;
constexpr int kDefaultDialogHeightMax    = 5000;
constexpr int kDefaultDialogWidthDefault  = 1000;
constexpr int kDefaultDialogHeightDefault = 800;

QSettings makeStandaloneSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QCoreApplication::organizationName().isEmpty()
                         ? QStringLiteral("GitBolt")
                         : QCoreApplication::organizationName(),
                     QCoreApplication::applicationName().isEmpty()
                         ? QStringLiteral("GitBolt")
                         : QCoreApplication::applicationName());
}

QSize readDefaultSize(QSettings& s)
{
    const int rawW = s.value(QStringLiteral("dialog/defaultWidth"),
                             kDefaultDialogWidthDefault).toInt();
    const int rawH = s.value(QStringLiteral("dialog/defaultHeight"),
                             kDefaultDialogHeightDefault).toInt();
    return QSize(qBound(kDefaultDialogWidthMin,  rawW, kDefaultDialogWidthMax),
                 qBound(kDefaultDialogHeightMin, rawH, kDefaultDialogHeightMax));
}

} // namespace

QSize SettingsService::defaultDialogSize() const
{
    return readDefaultSize(const_cast<QSettings&>(settings_));
}

void SettingsService::setDefaultDialogSize(QSize size)
{
    settings_.setValue(QStringLiteral("dialog/defaultWidth"),
                       qBound(kDefaultDialogWidthMin,
                              size.width(), kDefaultDialogWidthMax));
    settings_.setValue(QStringLiteral("dialog/defaultHeight"),
                       qBound(kDefaultDialogHeightMin,
                              size.height(), kDefaultDialogHeightMax));
    // We don't proactively clear saved per-dialog geometry here.
    // The "Restore previous dialog size" toggle in the settings
    // dialog gives the user direct control: if they want their
    // configured default to take effect on next open, they
    // uncheck the toggle. Clearing here would silently throw
    // away their drag-resized sizes — surprising behavior.
    emit settingsChanged();
}

QSize SettingsService::loadDefaultDialogSize()
{
    QSettings s = makeStandaloneSettings();
    return readDefaultSize(s);
}

bool SettingsService::restoreLastDialogSize() const
{
    return settings_.value(
        QStringLiteral("dialog/restoreLastSize"), true).toBool();
}

void SettingsService::setRestoreLastDialogSize(bool restore)
{
    settings_.setValue(QStringLiteral("dialog/restoreLastSize"), restore);
    emit settingsChanged();
}

namespace {

// Storage key for one dialog's last drag-resized geometry. Keep
// this in lockstep with applyConfiguredSize() — both readers and
// writers must agree on the path.
QString dialogGeomKey(const char* key)
{
    return QStringLiteral("layout/dialog/%1/geom")
        .arg(QLatin1StringView(key));
}

} // namespace

void SettingsService::applyConfiguredSize(QDialog* dlg, const char* key)
{
    if (!dlg || !key)
        return;

    QSettings s = makeStandaloneSettings();
    const bool restoreLast = s.value(
        QStringLiteral("dialog/restoreLastSize"), true).toBool();

    // Initial sizing: prefer the user's last drag-resized geometry
    // when the toggle is on AND we have something saved; otherwise
    // fall back to the configured default. restoreGeometry returns
    // false when the saved blob is malformed (corrupted settings,
    // schema-version mismatch) — we treat that the same as no save.
    bool sized = false;
    if (restoreLast) {
        const QByteArray geom =
            s.value(dialogGeomKey(key)).toByteArray();
        if (!geom.isEmpty())
            sized = dlg->restoreGeometry(geom);
    }
    if (!sized)
        dlg->resize(readDefaultSize(s));

    // Save geometry on close. We use QDialog::finished because it
    // fires whether the dialog was closed via accept(), reject(),
    // or the OS-level close button (closeEvent calls reject() by
    // default). Lambda captures `key` as a raw const char* — safe
    // since these come from string literals at every call site.
    QObject::connect(dlg, &QDialog::finished, dlg, [dlg, key](int) {
        QSettings ss = makeStandaloneSettings();
        ss.setValue(dialogGeomKey(key), dlg->saveGeometry());
    });

    // Standard close-window shortcut (Cmd+W on macOS, Ctrl+W
    // elsewhere) for every popup that adopts this helper. QDialog
    // only handles Escape out of the box; macOS users expect Cmd+W
    // to close the focused window, and it also gives UI automation
    // a deterministic one-keystroke close — the Close button
    // usually sits nested inside a QDialogButtonBox, where
    // accessibility tooling can't address it as a direct child of
    // the window. The default WindowShortcut context scopes the
    // shortcut to this dialog while it is the active window, so it
    // never collides with the main window's Cmd+W (Repository →
    // Close).
    auto* closeShortcut = new QShortcut(QKeySequence::Close, dlg);
    QObject::connect(closeShortcut, &QShortcut::activated,
                     dlg, &QDialog::close);
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
