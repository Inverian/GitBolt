#pragma once

#include "git/GitVersion.h"

#include <QColor>
#include <QFrame>
#include <QString>

#include <optional>

class QLabel;
class QPushButton;

namespace gitbolt::widgets {

/// What GitBolt tells the user about the git it runs (git::probeGit()):
/// there is none, it is too old for GitBolt, or it has known security
/// flaws (git-advisories.json), and what to do about it.
struct GitNotice {
    /// The git, its version and what is wrong with it, a line each: a
    /// dismissal (SettingsService::dismissGitNotice) lasts until one of
    /// them changes, a new git or a new advisory for this one.
    QString key;
    QString text;
};

/// The strip across the top of the window that shows a GitNotice, in
/// the note bars' amber (NoteBarColors): a note, not an error. Get Git…
/// opens git-scm.com's downloads; Dismiss hides it and says so
/// (dismissed()). Hidden until showNotice().
class GitNoticeBar : public QFrame {
    Q_OBJECT
public:
    /// Where Get Git… goes.
    static constexpr char kDownloadsUrl[] = "https://git-scm.com/downloads";

    explicit GitNoticeBar(QWidget* parent = nullptr);

    /// What to say about `probe` on `platform`; nothing for a git that
    /// is fine, or that didn't say what it is.
    static std::optional<GitNotice> noticeFor(
        const git::GitProbe& probe, git::GitPlatform platform,
        const git::GitAdvisories& table = git::builtInGitAdvisories());

    void showNotice(const GitNotice& notice);
    /// The notice shown, empty before the first.
    QString key() const { return key_; }
    QString text() const;

signals:
    /// Dismiss was clicked: the bar is hidden, and `key` is to be
    /// remembered.
    void dismissed(const QString& key);

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void applyColors();

    QLabel* message_ = nullptr;
    QPushButton* downloadButton_ = nullptr;
    QPushButton* dismissButton_ = nullptr;
    QString key_;
    QColor border_;
    bool applyingColors_ = false;
};

} // namespace gitbolt::widgets
