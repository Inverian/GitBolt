#include "dialogs/CloneDialog.h"
#include "conf/SettingsService.h"
#include "git/Repository.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <optional>

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
    urlEdit_->setPlaceholderText(QStringLiteral("https://github.com/user/repo.git"));
    layout->addWidget(urlEdit_);
    connect(urlEdit_, &QLineEdit::textChanged,
            this, &CloneDialog::onUrlChanged);

    // ----- Destination row -----
    layout->addWidget(new QLabel(tr("Clone to:"), this));
    auto* pathRow = new QHBoxLayout;
    pathRow->setSpacing(6);
    pathEdit_ = new QLineEdit(this);
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
// Run git_clone on a worker thread via QtConcurrent so the UI
// stays responsive. Disable the buttons + inputs while the clone
// is in flight; on success accept() the dialog (MainWindow then
// opens the repo); on failure show the error in statusLabel_ and
// re-enable the inputs so the user can fix the URL/path and retry.
// ---------------------------------------------------------------------------
void CloneDialog::onCloneClicked()
{
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
    // Refuse to overwrite an existing non-empty directory — git_clone
    // would also fail, but the error from libgit2 is opaque ("exists
    // and is not an empty directory") so we catch it up-front with a
    // friendlier message.
    if (QFileInfo::exists(path)) {
        QDir d(path);
        if (!d.isEmpty()) {
            setBusy(false, tr("Destination already exists and is not empty: %1").arg(path));
            return;
        }
    }

    setBusy(true, tr("Connecting to %1...").arg(url));
    progressBar_->setRange(0, 0);   // indeterminate until we hear back
    progressBar_->show();
    progressThrottle_.restart();
    lastPhase_ = -1;

    // ---- Progress callback (runs on the libgit2 worker thread) ----
    //
    // libgit2 fires transfer_progress MANY times per second. We
    // marshal each update to the GUI thread via the functor form of
    // QMetaObject::invokeMethod, which queues a lambda to run in
    // `this` object's event loop — safe to call from any thread.
    //
    // Safety: the Cancel and Clone buttons are both disabled while
    // the clone is in flight (see setBusy), so the dialog stays
    // alive for the entire clone. That means the raw `this` pointer
    // captured below is guaranteed valid for every callback.
    auto progressCb = [this](const git::CloneProgress& p) {
        // Worker thread — do NOT touch widgets here directly.
        QMetaObject::invokeMethod(this, [this, p]() {
            onProgressUpdate(p);
        }, Qt::QueuedConnection);
    };

    // QtConcurrent::run runs the lambda on a global thread pool
    // worker. The QFutureWatcher's `finished` signal is delivered
    // back to this thread (the GUI thread), so it's safe to touch
    // widgets from the slot. We return std::optional<QString>
    // rather than Result<Repository> because we don't actually
    // need the Repository handle here — MainWindow will reopen the
    // freshly-cloned directory via the normal openRepositoryAtPath
    // path. Empty optional = success; set optional = error message.
    auto* watcher = new QFutureWatcher<std::optional<QString>>(this);
    connect(watcher, &QFutureWatcher<std::optional<QString>>::finished,
            this, [this, watcher, path]() {
        const auto err = watcher->result();
        watcher->deleteLater();
        progressBar_->hide();
        if (err.has_value()) {
            setBusy(false, tr("Clone failed: %1").arg(*err));
            return;
        }
        clonedPath_ = path;
        accept();
    });

    watcher->setFuture(QtConcurrent::run(
        [url, path, progressCb = std::move(progressCb)]() -> std::optional<QString> {
            auto result = git::Repository::clone(url.toStdString(),
                                                 path.toStdString(),
                                                 progressCb);
            if (!result.ok())
                return QString::fromStdString(result.error().message());
            return std::nullopt;
        }));
}

// ---------------------------------------------------------------------------
// Progress slot — called on the GUI thread via the queued lambda
// posted from the worker-thread callback in onCloneClicked.
//
// libgit2 has three observable phases during a clone:
//
//   1. Receiving — objects and bytes stream in from the remote.
//      The progress bar tracks received_objects / total_objects and
//      the status line shows "Receiving objects: X% (n/total), Y MB".
//
//   2. Resolving — after the pack is fully received, libgit2 walks
//      the deltas to build the object index. The bar tracks
//      indexed_deltas / total_deltas.
//
//   3. CheckingOut — the working directory is populated. The bar
//      tracks completedSteps / totalSteps (files written).
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
            // indeterminate but show the byte count so the user
            // sees *something* moving.
            progressBar_->setRange(0, 0);
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
// Note: BOTH Ok (Clone) and Cancel get disabled while a clone is
// running. Cancel being disabled keeps the dialog alive for the
// entire clone — the worker thread holds a raw `this` pointer in
// its progress callback, and a mid-clone reject()/destroy would
// dangle that pointer. TODO: wire up a cancellation path that
// tells libgit2 to abort the fetch so users can bail out of a
// wrong-URL clone without waiting it out.
// ---------------------------------------------------------------------------
void CloneDialog::setBusy(bool busy, const QString& message)
{
    urlEdit_->setEnabled(!busy);
    pathEdit_->setEnabled(!busy);
    browseBtn_->setEnabled(!busy);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(!busy);
    buttons_->button(QDialogButtonBox::Cancel)->setEnabled(!busy);

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
    // most clone tools display transfer sizes.
    constexpr double K = 1000.0;
    if (n < 1000)              return QStringLiteral("%1 B").arg(n);
    if (n < 1000ull*1000)      return QStringLiteral("%1 KB").arg(n / K, 0, 'f', 1);
    if (n < 1000ull*1000*1000) return QStringLiteral("%1 MB").arg(n / (K*K), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(n / (K*K*K), 0, 'f', 2);
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
    const int colon = s.indexOf(QLatin1Char(':'));
    if (colon >= 0 && !s.startsWith(QStringLiteral("http"), Qt::CaseInsensitive))
        s = s.mid(colon + 1);
    // Strip query/fragment
    const int q = s.indexOf(QLatin1Char('?'));
    if (q >= 0) s.truncate(q);
    const int h = s.indexOf(QLatin1Char('#'));
    if (h >= 0) s.truncate(h);
    // Last path segment
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    const int slash = s.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        s = s.mid(slash + 1);
    if (s.endsWith(QStringLiteral(".git"), Qt::CaseInsensitive))
        s.chop(4);
    return s;
}

} // namespace gitbolt::dialogs
