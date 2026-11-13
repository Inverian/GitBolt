#pragma once

#include <QColor>
#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;
class QToolButton;

namespace gitbolt::widgets {

/// The strip across the top of the window that says the automatic
/// update check found a newer GitBolt (ui/UpdateController.h): "GitBolt
/// <v> is available." with View, which opens the update dialog, and a
/// dismiss ×. After an update it says so instead, with What's New. In
/// the note bars' amber (NoteBarColors), like GitNoticeBar: news, not an
/// error. It says only the version, which the updater parsed itself;
/// nothing of GitHub's text. Hidden until showVersion() or showUpdated().
///
/// Dismissing it lasts for the session (the controller remembers the
/// version); Skip This Version, in the dialog, is what lasts longer.
class UpdateNoticeBar : public QFrame {
    Q_OBJECT
public:
    explicit UpdateNoticeBar(QWidget* parent = nullptr);

    /// Shows the bar for `version` ("0.1.1").
    void showVersion(const QString& version);
    /// "GitBolt was updated to <version>." until dismissed or replaced.
    /// `restart`: this process ran while it went in, and isn't that
    /// version: the bar says to start GitBolt again.
    void showUpdated(const QString& version, bool restart = false);
    /// The version the bar offers (showVersion), empty before the first
    /// and while it says an update went in.
    QString version() const { return version_; }
    QString text() const;

signals:
    /// View was clicked: the bar is hidden, the dialog is to show.
    void viewRequested();
    /// What's New was clicked: the bar is hidden, and `version`'s release
    /// page is to open.
    void whatsNewRequested(const QString& version);
    /// × was clicked: the bar is hidden.
    void dismissed();

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void applyColors();

    QLabel* message_ = nullptr;
    QPushButton* viewButton_ = nullptr;
    QPushButton* whatsNewButton_ = nullptr;
    QToolButton* dismissButton_ = nullptr;
    QString version_;
    QString updated_;   ///< the version showUpdated() named
    QColor border_;
    bool applyingColors_ = false;
};

} // namespace gitbolt::widgets
