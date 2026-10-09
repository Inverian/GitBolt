#include "widgets/RepoOperationBar.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>

namespace gitbolt::widgets {

namespace {

// Light and dark versions of a note's amber: the bar has to stand out
// from the log under it without looking like an error.
struct BarColors {
    QColor background;
    QColor text;
    QColor border;
};

BarColors colorsFor(const QPalette& palette) {
    const bool dark = palette.color(QPalette::WindowText).lightness()
                      > palette.color(QPalette::Window).lightness();
    if (dark)
        return {QColor(0x3a, 0x30, 0x19), QColor(0xf0, 0xd4, 0x8a), QColor(0x6b, 0x5a, 0x2a)};
    return {QColor(0xff, 0xf4, 0xd6), QColor(0x5c, 0x44, 0x00), QColor(0xe8, 0xc7, 0x66)};
}

} // namespace

RepoOperationBar::RepoOperationBar(QWidget* parent)
    : QFrame(parent)
    , message_(new QLabel(this))
    , resolveButton_(new QPushButton(tr("Resolve Conflicts…"), this))
    , continueButton_(new QPushButton(tr("Continue"), this))
    , skipButton_(new QPushButton(tr("Skip"), this))
    , abortButton_(new QPushButton(tr("Abort"), this))
{
    setObjectName(QStringLiteral("repo.operationBar"));
    setAutoFillBackground(true);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 7);   // + the bottom line
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    row->addWidget(message_, 1);
    for (QPushButton* button : {resolveButton_, continueButton_, skipButton_, abortButton_}) {
        button->setAutoDefault(false);
        row->addWidget(button);
    }

    skipButton_->setToolTip(tr("Leave this commit out and carry on (git rebase --skip)"));
    resolveButton_->setToolTip(tr("Open the three-way conflict resolver"));

    connect(resolveButton_, &QPushButton::clicked, this, &RepoOperationBar::resolveRequested);
    connect(continueButton_, &QPushButton::clicked, this, &RepoOperationBar::continueRequested);
    connect(skipButton_, &QPushButton::clicked, this, &RepoOperationBar::skipRequested);
    connect(abortButton_, &QPushButton::clicked, this, &RepoOperationBar::abortRequested);

    applyColors();
    refresh();
}

void RepoOperationBar::setState(git::RepoState state, int conflicts) {
    state_ = state;
    conflicts_ = conflicts;
    refresh();
}

void RepoOperationBar::setBusy(bool busy) {
    busy_ = busy;
    refresh();
}

void RepoOperationBar::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    // A theme switch sets the application palette. (Not PaletteChange:
    // applyColors() sets this widget's own palette, which sends one.)
    if (event->type() == QEvent::ApplicationPaletteChange)
        applyColors();
}

void RepoOperationBar::applyColors() {
    const BarColors colors = colorsFor(QApplication::palette());
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, colors.background);
    palette.setColor(QPalette::WindowText, colors.text);
    setPalette(palette);
    border_ = colors.border;
    update();
}

void RepoOperationBar::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(QRect(0, height() - 1, width(), 1), border_);
}

void RepoOperationBar::refresh() {
    const bool conflicted = conflicts_ > 0;
    QString text;
    QString abortTip;
    switch (state_) {
    case git::RepoState::Rebase:
        text = conflicted
            ? tr("Rebase stopped on conflicts in %n file(s). Resolve them, then Continue.",
                 nullptr, conflicts_)
            : tr("A rebase is in progress. Continue when you're ready, or Abort to go back.");
        abortTip = tr("Stop the rebase and put the branch back where it was (git rebase --abort)");
        break;
    case git::RepoState::Merge:
        text = conflicted
            ? tr("Merge stopped on conflicts in %n file(s). Resolve them, then commit.",
                 nullptr, conflicts_)
            : tr("A merge is in progress. Commit to finish it.");
        abortTip = tr("Stop the merge and put the branch back where it was (git merge --abort)");
        break;
    case git::RepoState::CherryPick:
        text = conflicted
            ? tr("Cherry-pick stopped on conflicts in %n file(s). Resolve them, then commit.",
                 nullptr, conflicts_)
            : tr("A cherry-pick is in progress. Commit to finish it.");
        abortTip = tr("Stop the cherry-pick and put the branch back where it was "
                      "(git cherry-pick --abort)");
        break;
    case git::RepoState::Revert:
        text = conflicted
            ? tr("Revert stopped on conflicts in %n file(s). Resolve them, then commit.",
                 nullptr, conflicts_)
            : tr("A revert is in progress. Commit to finish it.");
        abortTip = tr("Stop the revert and put the branch back where it was (git revert --abort)");
        break;
    case git::RepoState::None:
    case git::RepoState::Other:
        setVisible(false);
        return;
    }

    const bool rebase = state_ == git::RepoState::Rebase;
    message_->setText(text);
    continueButton_->setText(rebase ? tr("Continue") : tr("Commit…"));
    continueButton_->setToolTip(
        conflicted ? tr("Resolve the conflicts first")
        : rebase   ? tr("Carry on with the rebase (git rebase --continue)")
                   : tr("Open the Commit dialog to finish it"));
    abortButton_->setToolTip(abortTip);

    resolveButton_->setVisible(conflicted);
    skipButton_->setVisible(rebase);
    resolveButton_->setEnabled(!busy_);
    continueButton_->setEnabled(!busy_ && !conflicted);
    skipButton_->setEnabled(!busy_);
    abortButton_->setEnabled(!busy_);
    setVisible(true);
}

} // namespace gitbolt::widgets
