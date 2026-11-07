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
/// dismiss ×. In the note bars' amber (NoteBarColors), like
/// GitNoticeBar: news, not an error. It says only the version, which
/// the updater parsed itself; nothing of GitHub's text. Hidden until
/// showVersion().
///
/// Dismissing it lasts for the session (the controller remembers the
/// version); Skip This Version, in the dialog, is what lasts longer.
class UpdateNoticeBar : public QFrame {
    Q_OBJECT
public:
    explicit UpdateNoticeBar(QWidget* parent = nullptr);

    /// Shows the bar for `version` ("0.1.1").
    void showVersion(const QString& version);
    /// The version shown last, empty before the first.
    QString version() const { return version_; }
    QString text() const;

signals:
    /// View was clicked: the bar is hidden, the dialog is to show.
    void viewRequested();
    /// × was clicked: the bar is hidden.
    void dismissed();

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void applyColors();

    QLabel* message_ = nullptr;
    QPushButton* viewButton_ = nullptr;
    QToolButton* dismissButton_ = nullptr;
    QString version_;
    QColor border_;
    bool applyingColors_ = false;
};

} // namespace gitbolt::widgets
