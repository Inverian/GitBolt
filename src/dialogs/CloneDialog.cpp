#include "dialogs/CloneDialog.h"
#include "conf/SettingsService.h"
#include "git/GitProcess.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace gitbolt::dialogs {

CloneDialog::CloneDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Clone Repository"));
    setModal(true);
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "clone");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(8);

    // ----- URL row -----
    layout->addWidget(new QLabel(tr("Repository URL:"), this));
    urlEdit_ = new QLineEdit(this);
    urlEdit_->setObjectName(QStringLiteral("clone.url"));
    urlEdit_->setPlaceholderText(QStringLiteral("https://github.com/user/repo.git"));
    layout->addWidget(urlEdit_);
    connect(urlEdit_, &QLineEdit::textChanged,
            this, &CloneDialog::onUrlChanged);

    // ----- Destination row -----
    layout->addWidget(new QLabel(tr("Clone to:"), this));
    auto* pathRow = new QHBoxLayout;
    pathRow->setSpacing(6);
    pathEdit_ = new QLineEdit(this);
    pathEdit_->setObjectName(QStringLiteral("clone.path"));
    pathEdit_->setPlaceholderText(defaultParentDir());
    pathRow->addWidget(pathEdit_);
    browseBtn_ = new QPushButton(tr("Browse..."), this);
    pathRow->addWidget(browseBtn_);
    layout->addLayout(pathRow);
    connect(browseBtn_, &QPushButton::clicked, this, &CloneDialog::onBrowse);
    // Mark the path as user-edited as soon as they type into it,
    // so we stop autofilling from the URL field.
    connect(pathEdit_, &QLineEdit::textEdited, this, [this](const QString&) {
        pathEditedByUser_ = true;
    });

    // ----- Status + progress (both hidden until clone starts) -----
    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    statusLabel_->setStyleSheet(QStringLiteral("QLabel { color: #666; }"));
    statusLabel_->hide();
    layout->addWidget(statusLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 0);     // starts indeterminate
    progressBar_->setTextVisible(false);
    progressBar_->setMinimumHeight(14);
    progressBar_->hide();
    layout->addWidget(progressBar_);

    layout->addStretch();

    // ----- Buttons -----
    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(tr("Clone"));
    buttons_->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted,
            this, &CloneDialog::onCloneClicked);
    connect(buttons_, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    // A kept clone (keptClonePath_) belongs to this URL and path: a
    // change to either makes the next click a fresh clone again.
    connect(urlEdit_, &QLineEdit::textChanged,
            this, &CloneDialog::forgetKeptClone);
    connect(pathEdit_, &QLineEdit::textChanged,
            this, &CloneDialog::forgetKeptClone);
}

CloneDialog::~CloneDialog()
{
    // reject()/closeEvent() keep the dialog open mid-clone, but the
    // app can still quit under the modal loop. Stop git, so the
    // worker — which the global thread pool waits for at exit —
    // returns promptly instead of finishing the whole download.
    if (cloning_ && cancelFlag_)
        cancelFlag_->store(true);
}

// ---------------------------------------------------------------------------
// Auto-fill the destination from the URL: take the last path
// segment, strip a trailing ".git", and join with the default
// parent directory. Stops as soon as the user manually edits the
// path field — see pathEditedByUser_.
// ---------------------------------------------------------------------------
void CloneDialog::onUrlChanged(const QString& url)
{
    if (pathEditedByUser_)
        return;
    const QString name = repoNameFromUrl(url);
    if (name.isEmpty()) {
        pathEdit_->setText(QString{});
        return;
    }
    pathEdit_->setText(QDir(defaultParentDir()).filePath(name));
}

void CloneDialog::onBrowse()
{
    const QString start = pathEdit_->text().isEmpty()
        ? defaultParentDir()
        : QFileInfo(pathEdit_->text()).absolutePath();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select destination parent directory"), start);
    if (dir.isEmpty())
        return;

    // The picker returns the parent directory; append the repo
    // name (from the current URL) so the user gets a sensible
    // <parent>/<repo> destination by default.
    const QString name = repoNameFromUrl(urlEdit_->text());
    pathEdit_->setText(name.isEmpty() ? dir : QDir(dir).filePath(name));
    pathEditedByUser_ = true;
}

// ---------------------------------------------------------------------------
// Run `git clone` on a worker thread via QtConcurrent so the UI
// stays responsive. Disable the buttons + inputs while the clone
// is in flight; on success accept() the dialog (MainWindow then
// opens the repo); on failure show the error in statusLabel_ and
// re-enable the inputs so the user can fix the URL/path and retry.
//
// The git CLI — not libgit2 — so clone authenticates like push /
// pull / fetch: the user's credential helpers (osxkeychain, Git
// Credential Manager), ssh config / agent / known_hosts and
// core.sshCommand, with GitBolt's askpass prompt as the fallback.
// ---------------------------------------------------------------------------
void CloneDialog::onCloneClicked()
{
    // The last clone fetched but failed to check out, and git kept
    // it: the button reads "Open Repository", and MainWindow opens
    // the repository as it is.
    if (!keptClonePath_.isEmpty()) {
        clonedPath_ = keptClonePath_;
        accept();
        return;
    }

    const QString url  = urlEdit_->text().trimmed();
    const QString path = pathEdit_->text().trimmed();

    if (url.isEmpty()) {
        setBusy(false, tr("Please enter a repository URL."));
        return;
    }
    if (path.isEmpty()) {
        setBusy(false, tr("Please choose a destination directory."));
        return;
    }
    // Refuse to overwrite an existing non-empty directory — git would
    // also fail, but we catch it up-front with a friendlier message.
    // Hidden entries count: git refuses a directory holding nothing
    // but a .DS_Store, too.
    const QFileInfo target(path);
    if (target.exists()
        && (!target.isDir()
            || !QDir(path).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot
                                   | QDir::Hidden | QDir::System))) {
        setBusy(false, tr("Destination already exists and is not empty: %1").arg(path));
        return;
    }

    setBusy(true, tr("Connecting to %1...").arg(url));
    progressBar_->setRange(0, 0);   // indeterminate until we hear back
    progressBar_->show();
    progressThrottle_.restart();
    lastPhase_ = -1;

    cloning_ = true;
    cancelRequested_ = false;
    cancelFlag_ = std::make_shared<std::atomic<bool>>(false);

    // ---- Progress callback (runs on the clone worker thread) ----
    //
    // GitProcess::clone parses git's progress lines on the worker and
    // hands each update here. We marshal it to the GUI thread via the
    // functor form of QMetaObject::invokeMethod, which queues a lambda
    // on the application object's (GUI) thread — safe from any thread.
    //
    // Safety: while a clone is in flight reject()/closeEvent() turn
    // into "request cancel" instead of closing, so the dialog normally
    // outlives the worker. For the one way around that — the app
    // quitting under the modal loop — the lambda carries a QPointer
    // and checks it on the GUI thread, so an update landing after
    // the dialog is gone is dropped instead of reaching a dead object.
    const QPointer<CloneDialog> self(this);
    auto progressCb = [self](const git::CloneProgress& p) {
        // Worker thread — do NOT touch widgets (or `self`) here.
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, p]() {
            if (self)
                self->onProgressUpdate(p);
        }, Qt::QueuedConnection);
    };

    // QtConcurrent::run runs the lambda on a global thread pool
    // worker. The QFutureWatcher's `finished` signal is delivered
    // back to this thread (the GUI thread), so it's safe to touch
    // widgets from the slot. Nothing comes back on success —
    // MainWindow opens the freshly-cloned directory via the normal
    // openRepositoryAtPath path.
    auto* watcher = new QFutureWatcher<git::Result<void>>(this);
    connect(watcher, &QFutureWatcher<git::Result<void>>::finished,
            this, [this, watcher, path]() {
        const git::Result<void> outcome = watcher->result();
        watcher->deleteLater();
        progressBar_->hide();
        cloning_ = false;

        if (!outcome.ok()) {
            const git::GitError& err = outcome.error();
            const QString message = QString::fromStdString(err.message());
            if (err.code() == git::GitErrorCode::CheckoutFailed) {
                // Fetched, but the checkout failed, and git kept the
                // repository — even when a Cancel came too late to
                // stop it. The message says where it is and how git
                // suggests finishing the checkout; offer to open it.
                setBusy(false, message);
                keptClonePath_ = path;
                buttons_->button(QDialogButtonBox::Ok)->setText(tr("Open Repository"));
            } else if (cancelRequested_) {
                // GitProcess::clone has already put the destination
                // back the way it was (removed, or emptied if it
                // pre-existed), so a retry — same path, fixed URL —
                // passes the up-front empty-directory check.
                setBusy(false, tr("Clone cancelled."));
            } else {
                setBusy(false, tr("Clone failed: %1").arg(message));
            }
            cancelRequested_ = false;
            return;
        }
        clonedPath_ = path;
        accept();
    });

    // Credentials need no wiring here: a private HTTPS remote is
    // answered by the user's credential helper, and failing that git
    // (or ssh, for a key passphrase / host key) runs GitBolt's askpass
    // prompt — see GitProcess::applyEnvironment.
    watcher->setFuture(QtConcurrent::run(
        [url, path, progressCb = std::move(progressCb),
         flag = cancelFlag_]() -> git::Result<void> {
            return git::GitProcess::clone(url.toStdString(),
                                          path.toStdString(),
                                          progressCb,
                                          flag);
        }));
}

// Back to "Clone" once the URL or path no longer names the kept clone.
void CloneDialog::forgetKeptClone()
{
    if (keptClonePath_.isEmpty())
        return;
    keptClonePath_.clear();
    buttons_->button(QDialogButtonBox::Ok)->setText(tr("Clone"));
}

// ---------------------------------------------------------------------------
// Cancellation. The Cancel button stays ENABLED during a clone (see
// setBusy) and routes here via reject(): first activation flips the
// shared atomic flag, which the worker checks every ~50 ms while git
// runs — it then stops git and every process git started, restores
// the destination, and the finished handler above reports "Clone
// cancelled.". The dialog itself only closes once the worker is done.
// ---------------------------------------------------------------------------
void CloneDialog::requestCancel()
{
    if (!cloning_ || cancelRequested_)
        return;
    cancelRequested_ = true;
    if (cancelFlag_)
        cancelFlag_->store(true);
    statusLabel_->setText(tr("Cancelling…"));
    // One shot — further Cancel clicks while the abort drains do
    // nothing (the button also visually disables).
    buttons_->button(QDialogButtonBox::Cancel)->setEnabled(false);
}

void CloneDialog::reject()
{
    if (cloning_) {
        requestCancel();
        return;     // stay open until the worker finishes
    }
    QDialog::reject();
}

void CloneDialog::closeEvent(QCloseEvent* event)
{
    if (cloning_) {
        requestCancel();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Progress slot — called on the GUI thread via the queued lambda
// posted from the worker-thread callback in onCloneClicked.
//
// git reports three observable phases during a clone (parsed from
// its --progress output by CloneProgressParser):
//
//   1. Receiving — objects and bytes stream in from the remote.
//      The progress bar tracks receivedObjects / totalObjects and
//      the status line shows "Receiving objects: X% (n/total), Y MB".
//      Before the first object (connecting, or the server still
//      counting / compressing) the bar stays indeterminate.
//
//   2. Resolving — after the pack is fully received, index-pack
//      resolves the deltas. The bar tracks indexedDeltas /
//      totalDeltas.
//
//   3. CheckingOut — the working directory is populated. The bar
//      tracks completedSteps / totalSteps (files written). git only
//      reports this phase when the checkout takes more than ~2 s.
//
// Updates are throttled to ~50 ms so the bar animates smoothly on
// fast clones without flooding the event loop, but every phase
// transition forces an immediate paint so the user sees the switch
// from "Receiving" → "Resolving" → "Checking out" promptly.
// ---------------------------------------------------------------------------
void CloneDialog::onProgressUpdate(gitbolt::git::CloneProgress p)
{
    const int phaseInt = static_cast<int>(p.phase);
    const bool phaseChanged = (phaseInt != lastPhase_);
    // Throttle: skip if same phase and fewer than ~50 ms since last paint
    if (!phaseChanged && progressThrottle_.elapsed() < 50)
        return;
    lastPhase_ = phaseInt;
    progressThrottle_.restart();

    switch (p.phase) {
    case git::CloneProgress::Phase::Receiving: {
        if (p.totalObjects > 0) {
            const int pct = static_cast<int>(
                (p.receivedObjects * 100ull) / p.totalObjects);
            progressBar_->setRange(0, 100);
            progressBar_->setValue(pct);
            statusLabel_->setText(
                tr("Receiving objects: %1% (%2/%3) — %4")
                    .arg(pct)
                    .arg(p.receivedObjects)
                    .arg(p.totalObjects)
                    .arg(humanBytes(p.receivedBytes)));
        } else {
            // Remote hasn't sent the object count yet — stay
            // indeterminate. Keep the "Connecting to <url>..." line
            // until bytes actually arrive (the server may spend a
            // while counting / compressing first), then show the
            // byte count so the user sees *something* moving.
            progressBar_->setRange(0, 0);
            if (p.receivedBytes > 0)
                statusLabel_->setText(
                    tr("Connecting... %1 received").arg(humanBytes(p.receivedBytes)));
        }
        break;
    }
    case git::CloneProgress::Phase::Resolving: {
        if (p.totalDeltas > 0) {
            const int pct = static_cast<int>(
                (p.indexedDeltas * 100ull) / p.totalDeltas);
            progressBar_->setRange(0, 100);
            progressBar_->setValue(pct);
            statusLabel_->setText(
                tr("Resolving deltas: %1% (%2/%3)")
                    .arg(pct)
                    .arg(p.indexedDeltas)
                    .arg(p.totalDeltas));
        }
        break;
    }
    case git::CloneProgress::Phase::CheckingOut: {
        if (p.totalSteps > 0) {
            const int pct = static_cast<int>(
                (p.completedSteps * 100ull) / p.totalSteps);
            progressBar_->setRange(0, 100);
            progressBar_->setValue(pct);
            statusLabel_->setText(
                tr("Checking out files: %1% (%2/%3)")
                    .arg(pct)
                    .arg(p.completedSteps)
                    .arg(p.totalSteps));
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Toggle the inputs and the buttons between idle and "in flight",
// and show a status line. Empty `message` hides the status row.
//
// Cancel stays ENABLED during a clone — it's the cancellation
// trigger (see requestCancel). reject()/closeEvent() are overridden
// so Cancel / Esc / the window close button all request a cancel
// instead of closing; the dialog itself only closes once the worker
// has actually stopped and the partial clone is cleaned up.
// ---------------------------------------------------------------------------
void CloneDialog::setBusy(bool busy, const QString& message)
{
    urlEdit_->setEnabled(!busy);
    pathEdit_->setEnabled(!busy);
    browseBtn_->setEnabled(!busy);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(!busy);
    buttons_->button(QDialogButtonBox::Cancel)->setEnabled(true);

    if (message.isEmpty()) {
        statusLabel_->hide();
        statusLabel_->clear();
        return;
    }
    statusLabel_->setText(message);
    statusLabel_->setStyleSheet(busy
        ? QStringLiteral("QLabel { color: #1f6feb; }")    // info / in-flight
        : QStringLiteral("QLabel { color: #d73a49; }"));  // error / idle
    statusLabel_->show();
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------
QString CloneDialog::defaultParentDir()
{
    // Prefer ~/Developer if it exists (matches the user's existing
    // habit), otherwise fall back to the standard Documents folder.
    const QString dev = QDir::homePath() + QStringLiteral("/Developer");
    if (QFileInfo::exists(dev))
        return dev;
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QString CloneDialog::humanBytes(quint64 n)
{
    // Small helper for the status line — "12.3 MB", "843 KB", etc.
    // Stays in decimal (1000-based) units; matches how GitHub and
    // most clone tools display transfer sizes. The double conversions
    // are exact below 2^53 bytes (~9 PB) — far past any real clone.
    constexpr double K = 1000.0;
    if (n < 1000)              return QStringLiteral("%1 B").arg(n);
    if (n < 1000ull*1000)      return QStringLiteral("%1 KB").arg(static_cast<double>(n) / K, 0, 'f', 1);
    if (n < 1000ull*1000*1000) return QStringLiteral("%1 MB").arg(static_cast<double>(n) / (K*K), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(static_cast<double>(n) / (K*K*K), 0, 'f', 2);
}

QString CloneDialog::repoNameFromUrl(const QString& url)
{
    // Take the last path segment of the URL and strip a trailing
    // ".git" suffix. Handles both `https://...` and `git@host:org/repo.git`
    // forms — for the SSH form we treat ":" as a separator too.
    QString s = url.trimmed();
    if (s.isEmpty())
        return {};
    // SSH-style: split off everything before the first ":"
    const qsizetype colon = s.indexOf(QLatin1Char(':'));
    if (colon >= 0 && !s.startsWith(QStringLiteral("http"), Qt::CaseInsensitive))
        s = s.mid(colon + 1);
    // Strip query/fragment
    const qsizetype q = s.indexOf(QLatin1Char('?'));
    if (q >= 0) s.truncate(q);
    const qsizetype h = s.indexOf(QLatin1Char('#'));
    if (h >= 0) s.truncate(h);
    // Last path segment
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    const qsizetype slash = s.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        s = s.mid(slash + 1);
    if (s.endsWith(QStringLiteral(".git"), Qt::CaseInsensitive))
        s.chop(4);
    return s;
}

} // namespace gitbolt::dialogs
