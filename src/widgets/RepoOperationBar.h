#pragma once

#include "git/Merge.h"

#include <QFrame>

class QLabel;
class QPushButton;

namespace gitbolt::widgets {

/// The strip across the top of the repository view while git is in
/// the middle of a rebase, merge, cherry-pick or revert: what is going
/// on, and the buttons that finish it. Hidden otherwise.
///
/// A rebase that stopped — on a conflict, or at an `edit` — used to be
/// finishable only from the Rebase dialog that started it, if that
/// dialog was still open; closing it, starting the rebase with the
/// dialog's own Rebase button, or restarting GitBolt left no way on.
/// The bar follows the repository instead, wherever the operation came
/// from (a terminal included).
///
///   Rebase:                 [Resolve Conflicts…] [Continue] [Skip] [Abort]
///   Merge / cherry-pick /
///   revert:                 [Resolve Conflicts…] [Commit…]         [Abort]
///   …stopped in a sequence
///   of several commits:     [Resolve Conflicts…] [Commit…]  [Skip] [Abort]
///   Cherry-pick / revert of
///   several commits, paused
///   between two:                                 [Continue]        [Abort]
///   …paused before picking
///   the commit it stopped at:                               [Skip] [Abort]
///
/// Resolve Conflicts… shows only while files are conflicted, and
/// Continue / Commit… wait until none are. Skip shows for a rebase, and
/// for a sequence of several cherry-picks or reverts only where git's
/// --skip would work (Repository::sequenceSkippable()): at a commit the
/// sequence stopped at, conflicted or empty, but not once that commit
/// has been committed, where git has "nothing to skip". Paused with
/// the commit it stopped at never picked (sequenceSkippable() while
/// paused), git's --continue would leave that commit out just as --skip
/// does, so only Skip is offered.
class RepoOperationBar : public QFrame {
    Q_OBJECT
public:
    explicit RepoOperationBar(QWidget* parent = nullptr);

    /// From GitService::repoStateReady: shows, updates or hides the
    /// bar. RepoState::Other (am, bisect) has nothing to offer here.
    /// `sequencePaused`: a cherry-pick or revert of several commits is
    /// paused between two, with nothing to commit; Continue carries it
    /// on, as for a rebase. `sequenceSkippable`: such a sequence, paused
    /// or stopped at a commit, can leave that commit out (Skip).
    void setState(git::RepoState state, int conflicts, bool sequencePaused = false,
                  bool sequenceSkippable = false);
    git::RepoState state() const { return state_; }
    bool sequencePaused() const { return sequencePaused_; }
    bool sequenceSkippable() const { return sequenceSkippable_; }

    /// While a step runs, every button is off, so that a second click
    /// can't start another git command on top of it.
    void setBusy(bool busy);
    bool isBusy() const { return busy_; }

signals:
    void resolveRequested();
    /// Rebase: `git rebase --continue`; a paused sequence: its
    /// --continue. Otherwise: commit, which concludes the merge /
    /// cherry-pick / revert.
    void continueRequested();
    /// Rebase, or a sequence that can skip: its --skip.
    void skipRequested();
    void abortRequested();

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void refresh();
    void applyColors();

    git::RepoState state_ = git::RepoState::None;
    int conflicts_ = 0;
    bool sequencePaused_ = false;
    bool sequenceSkippable_ = false;
    bool busy_ = false;
    bool applyingColors_ = false;
    QColor border_;

    QLabel* message_ = nullptr;
    QPushButton* resolveButton_ = nullptr;
    QPushButton* continueButton_ = nullptr;
    QPushButton* skipButton_ = nullptr;
    QPushButton* abortButton_ = nullptr;
};

} // namespace gitbolt::widgets
