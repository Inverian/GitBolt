#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QSettings>
#include <QStringList>

namespace gitbolt::conf {

class SettingsService : public QObject {
    Q_OBJECT
public:
    explicit SettingsService(QObject* parent = nullptr);

    // Recent repositories
    QStringList recentRepositories() const;
    void addRecentRepository(const QString& path);
    void removeRecentRepository(const QString& path);
    void clearRecentRepositories();

    /// Timestamp of the last time the user opened each recent repository.
    /// Keys are the canonical absolute paths returned by
    /// `recentRepositories()`; values are UTC QDateTimes. Paths that
    /// have never been opened since this key was introduced (i.e.
    /// pre-existing entries from older builds) will be missing from
    /// the map — the caller should treat that as "unknown".
    QHash<QString, QDateTime> recentAccessTimes() const;

    // Recent repositories — display / retention preferences.
    //
    // The cap used by addRecentRepository() when trimming the list.
    // Existing entries are NOT proactively pruned when the cap is
    // lowered — the list naturally shrinks on the next add. Clamped
    // to [1, 200] to keep pathological values from breaking the UI.
    int maxRecentRepositories() const;
    void setMaxRecentRepositories(int count);

    /// Sort the recent-repos list alphabetically by path basename
    /// when true. When false, the list is ordered by most-recent-use
    /// (the default, same as traditional MRU behavior).
    bool sortRecentAlphabetically() const;
    void setSortRecentAlphabetically(bool sort);

    /// Strategy for abbreviating a long repo path in the UI. These
    /// map to the three options shown in GitExtensions' Recent
    /// Repositories settings page:
    ///   0 = None            — show the path as-is
    ///   1 = MiddleEllipsis  — collapse the middle with "…"
    ///   2 = SignificantDir  — show only the deepest directory name
    enum class RecentShortening {
        None = 0,
        MiddleEllipsis = 1,
        SignificantDir = 2,
    };
    RecentShortening recentShorteningStrategy() const;
    void setRecentShorteningStrategy(RecentShortening strategy);

    // UI — code font
    QFont codeFont() const;
    void setCodeFont(const QFont& font);
    int codeFontSize() const;
    void setCodeFontSize(int size);

    // Git defaults
    QString defaultRemote() const;
    void setDefaultRemote(const QString& remote);

    /// UI theme persisted across launches: "Light", "Dark", or
    /// "System" (default). main.cpp restores it before the window
    /// shows and persists every ThemeService::themeChanged.
    QString theme() const;
    void setTheme(const QString& name);

    // Behaviour
    int tabSize() const;
    void setTabSize(int size);
    bool showWhitespace() const;
    void setShowWhitespace(bool show);

    // UI Design — RepositoryView's vertical splitter default. The
    // percent is "how much of the repo view's right column the
    // bottom inspector pane should occupy at first launch (and
    // when the user resets it from the settings dialog)". The top
    // pane (revision graph) gets (100 - percent) percent. Clamped
    // to [10, 90] so the user can't starve either pane completely.
    // Default is 40 — the bottom inspector tabs get 40% of the
    // vertical space, leaving 60% for the revision graph / log.
    int bottomPanePercent() const;
    void setBottomPanePercent(int percent);

    // Window state persistence
    void saveWindowGeometry(const QByteArray& geometry);
    QByteArray restoreWindowGeometry() const;
    void saveWindowState(const QByteArray& state);
    QByteArray restoreWindowState() const;

    // Startup window size — the dimensions the main window opens at
    // when there's no saved geometry (first launch) OR when
    // `restoreLastWindowSize()` is false. Defaults scale to ~80% of
    // the primary screen so the values feel right both on small
    // laptops and on ultrawides, then clamp to [1280, 2400] ×
    // [800, 1500] so we never pick something silly. Setters also
    // clamp to wider hard-bounds so a typoed huge value can't break
    // the UI.
    int startupWindowWidth() const;
    void setStartupWindowWidth(int width);
    int startupWindowHeight() const;
    void setStartupWindowHeight(int height);

    // When true (the default), the main window restores its previous
    // geometry on launch via QMainWindow::restoreGeometry, so the
    // user's last-dragged size persists across launches. When false,
    // the window always opens at the configured `startupWindowWidth`
    // × `startupWindowHeight`, ignoring any saved geometry — useful
    // for users who want a consistent first-frame on every launch.
    bool restoreLastWindowSize() const;
    void setRestoreLastWindowSize(bool restore);

    // ----- Dialog default size -----
    //
    // One W × H pair shared across every popup dialog the user
    // can resize (Commit, Clone, Tag, Stash, Rebase, Reflog, …).
    // Each dialog calls applyConfiguredSize() in its constructor
    // and the helper handles initial sizing + per-dialog
    // persistence. Dialogs whose layout needs more space than the
    // configured default auto-grow via Qt's minimum-size-hint
    // propagation, so a small global value doesn't break
    // content-heavy dialogs.
    //
    // Setters clamp to wide hard-bounds for typo safety; the
    // settings UI applies a tighter functional minimum.
    QSize defaultDialogSize() const;
    void setDefaultDialogSize(QSize size);

    // Static convenience — reads the configured default size
    // without requiring a SettingsService instance.
    static QSize loadDefaultDialogSize();

    // Whether each dialog should remember its drag-resized
    // geometry across opens. ON (default): on open, restore the
    // dialog's last size from layout/dialog/<key>/geom if one is
    // saved, else fall back to the configured default. OFF: every
    // open uses the configured default, ignoring any saved size.
    // Mirrors restoreLastWindowSize for the main window.
    bool restoreLastDialogSize() const;
    void setRestoreLastDialogSize(bool restore);

    // One-stop helper called by every resizable popup dialog from
    // its constructor. Handles: (1) the initial resize — restoring
    // a previously-saved geometry if `restoreLastDialogSize()` is
    // on and a saved size exists for `key`, otherwise applying
    // the configured default; (2) wiring up the dialog's
    // `finished` signal so the geometry is saved on close; and
    // (3) a standard close-window shortcut (Cmd+W on macOS,
    // Ctrl+W elsewhere) so every popup closes the way macOS users
    // expect — and the way UI automation can rely on.
    // The geometry save happens regardless of the toggle's current
    // state, so toggling the setting back on later still picks up
    // the most recent drag-resized size.
    static void applyConfiguredSize(class QDialog* dlg, const char* key);

    // Layout persistence — generic splitter + dialog geometry helpers
    // keyed under "layout/<key>". All keys used by the app carry a
    // schema-version suffix (e.g. ".../v1") so future layout changes
    // can bump the suffix without stomping on stored state from older
    // versions. Keys expected by the app:
    //   layout/repoSplitterH/v1     — RepositoryView outer H splitter
    //   layout/repoSplitterV/v1     — RepositoryView inner V splitter
    //   layout/diffSplitter/v1      — Diff tab internal splitter
    //   layout/commitDialogGeom/v1  — CommitDialog geometry
    //   layout/commitSplitter/v1    — CommitDialog staging|editor splitter
    void saveSplitterState(const QString& key, const QByteArray& state);
    QByteArray restoreSplitterState(const QString& key) const;
    void saveDialogGeometry(const QString& key, const QByteArray& geometry);
    QByteArray restoreDialogGeometry(const QString& key) const;

    // Raw access for ad-hoc keys
    QVariant value(const QString& key, const QVariant& defaultValue = {}) const;
    void setValue(const QString& key, const QVariant& value);

signals:
    void settingsChanged();

private:
    // Fallback cap used when the user hasn't set one yet. The
    // configurable `maxRecentRepositories()` setter overrides this
    // for any new add-to-recents operation.
    static constexpr int kDefaultMaxRecentRepositories = 10;
    QSettings settings_;
};

} // namespace gitbolt::conf
