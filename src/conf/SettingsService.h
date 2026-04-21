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

    // Recent repositories (max 10)
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
    static constexpr int kMaxRecentRepositories = 10;
    QSettings settings_;
};

} // namespace gitbolt::conf
