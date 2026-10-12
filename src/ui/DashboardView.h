#pragma once

#include <QList>
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;
class QTreeWidget;

namespace gitbolt::conf {
class SettingsService;
} // namespace gitbolt::conf

namespace gitbolt::ui {

class DashboardView : public QWidget {
    Q_OBJECT
public:
    explicit DashboardView(conf::SettingsService* settings, QWidget* parent = nullptr);

    /// Refresh the recent-repositories list from SettingsService.
    void refreshRecentList();

signals:
    void openRepositoryRequested(const QString& path);
    void cloneRequested();
    void initRequested(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    /// Watch recentList_ for QEvent::Resize. DashboardView's own
    /// resizeEvent fires BEFORE the nested tree widget gets laid out,
    /// so its viewport() width is stale at that moment; watching the
    /// tree directly catches the moment it actually gets its real size.
    bool eventFilter(QObject* watched, QEvent* event) override;
    /// The decorative bolt logo isn't in the main layout — it's a
    /// floating child anchored to the bottom-right corner of the
    /// view. We reposition it on every resize so it stays in the
    /// corner regardless of window size.
    void resizeEvent(QResizeEvent* event) override;
    /// A theme switch: the cards' descriptions take the theme's colour
    /// (applyCardColors()).
    void changeEvent(QEvent* event) override;

private:
    void setupUi();
    /// Apply the percentage split to the tree's columns. Pulled out of
    /// eventFilter() so it can also be invoked after refresh when the
    /// item set changes (scrollbar visibility may shift the viewport).
    void updateRecentColumnWidths();
    /// Kick off a background probe for one repo and populate the
    /// corresponding row's columns when it completes. Row lookup is by
    /// path stashed in column 0's UserRole only, never by index or
    /// pointer: a rebuild in between deletes the rows, and a new row of
    /// another repository can get a deleted one's address.
    void probeAndPopulate(const QString& path);
    QPushButton* createActionCard(const QString& title, const QString& iconText,
                                  const QString& description);
    /// The cards' descriptions in the theme's colour. palette(mid), a
    /// border's grey, read at 1.4:1 on the dark theme's cards (#3c3c3c)
    /// and at 1.7:1 on the light theme's window.
    void applyCardColors();
    QList<QLabel*> cardDescriptions_;

    conf::SettingsService* settings_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QPushButton* openBtn_ = nullptr;
    QPushButton* cloneBtn_ = nullptr;
    QPushButton* initBtn_ = nullptr;
    /// Six-column table of recent repositories:
    ///   0 Name     — QFileInfo::fileName() of the path
    ///   1 Branch   — current branch name, or "(detached)"
    ///   2 Status   — dirty file count, e.g. "●3" or blank if clean
    ///   3 ↑↓       — ahead/behind vs. upstream, e.g. "↑2 ↓1" or "—"
    ///   4 Accessed — relative time since last opened in GitBolt
    ///   5 Committed — relative time since HEAD committer timestamp
    ///   6 Path     — full absolute path
    /// Columns 1-5 are populated asynchronously per-row via a worker
    /// thread pool (see probeAndPopulate) so slow repos don't block
    /// the dashboard paint.
    QTreeWidget* recentList_ = nullptr;
    QPushButton* clearRecentBtn_ = nullptr;
    QLabel* noRecentLabel_ = nullptr;

    // Floating decorative bolt — parented to DashboardView (not to
    // any layout) so it sits "on top of" the dashboard at an
    // absolute position that resizeEvent keeps pinned to the
    // bottom-right corner regardless of window size.
    QWidget* cornerBolt_ = nullptr;
};

} // namespace gitbolt::ui
