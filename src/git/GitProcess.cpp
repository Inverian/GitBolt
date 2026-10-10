#include "git/GitProcess.h"
#include "git/GitProcessLog.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>
#include <optional>
#include <algorithm>
#include <sstream>
#include <string_view>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Windows 10 SDKs name it; spelled out for configurations that
// target an older _WIN32_WINNT.
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
#define PROC_THREAD_ATTRIBUTE_JOB_LIST 0x0002000D
#endif
#else
#include <csignal>
#include <unistd.h>
#endif

namespace gitbolt::git {

namespace {

// Where interactiveRebase() leaves the messages of the commits a plan
// rewords: a folder in git's own rebase state, so that it lives exactly
// as long as the rebase does — git deletes it when the rebase finishes
// or is aborted. One file per commit, named by its full hash, and the
// rebase's comment character (kCommentChar).
const QString kRewordMessages = QStringLiteral("gitbolt-messages");

// The editor every git command gets (see applyEnvironment). It leaves
// the file alone, as ":" would, except for the commit message of a
// commit a plan rewords: git's `done` list ends with that commit's
// todo line ("reword <hash> <subject>") while git asks for its
// message, the first time and again after a conflict stopped it. A
// commit that is skipped never gets asked about, so its message can't
// end up on another commit. git runs this through sh, with the file's
// path appended — "$1" here.
const QString kEditor = QStringLiteral(
    "d=$(git rev-parse --git-path rebase-merge) && "
    "m=\"$d/%1/$(awk 'END { print $2 }' \"$d/done\" 2>/dev/null)\" && "
    "test -f \"$m\" && exec cp \"$m\" \"$1\"; :").arg(kRewordMessages);

// The file in kRewordMessages that holds the comment character a rebase
// runs with (see commentCharFor). No commit hash can be called that.
const QString kCommentChar = QStringLiteral("comment-char");

// Every step of a rebase may replay the rest of the plan in one go, and
// each reword in it runs the commit hooks. 30 s, run()'s default, cut
// a Continue off mid-plan, git killed and its index.lock left behind.
constexpr int kRebaseTimeoutMs = 300000;

// git strips the lines that start with core.commentChar from every
// message it has had edited: a reword's, which kEditor writes, and a
// squash's, which git puts together from the messages it combines.
// With the default '#', a reworded "#123 Fix login" came out empty, so
// git stopped and the commit kept its old message, and a body's
// "# Notes" or Markdown heading went missing without a word. So a
// rebase runs with a comment character that starts no line of any
// message it could put in a commit: the first of the characters git
// itself picks from for core.commentChar=auto that is free, or '\0'
// when none is.
char commentCharFor(const std::vector<std::string>& messages) {
    const auto startsALine = [](const std::string& text, char c) {
        for (size_t at = 0; at < text.size(); ++at) {
            if (text[at] == c)
                return true;
            at = text.find('\n', at);
            if (at == std::string::npos)
                break;
        }
        return false;
    };
    for (const char c : std::string_view("#;@!$%^&|:")) {
        if (std::none_of(messages.begin(), messages.end(),
                         [&](const std::string& m) { return startsALine(m, c); }))
            return c;
    }
    return '\0';
}

} // namespace

GitProcess::GitProcess(const std::string& workingDirectory)
    : workDir_(workingDirectory), gitPath_(findGitExecutable()) {}

void GitProcess::applyEnvironment(QProcess& process) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // The child has no tty, so a git that decides to prompt (HTTPS
    // remote with no cached credential, ssh host-key confirmation)
    // used to block until waitForFinished's timeout killed it —
    // 120 s of frozen "Pushing…" ending in a meaningless timeout.
    // Forbidding prompts makes those cases fail immediately with
    // git's own stderr explanation. Credential HELPERS (osxkeychain,
    // libsecret, manager-core) are unaffected — only terminal
    // prompting is disabled.
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    // No tty, so no editor either: `git rebase --continue` opens one
    // for the message of the commit it makes, and so would any command
    // that wants a message edited. vi waited on the pipe until the
    // timeout killed git mid-operation, and a GUI core.editor popped
    // up out of nowhere. kEditor keeps the text git prepared, as ":"
    // (git's own "no editor") would, except where a rebase plan gave
    // a commit a new message.
    env.insert(QStringLiteral("GIT_EDITOR"), kEditor);

    // When git (or ssh underneath it) does need a credential and no
    // helper supplies one, route the question to a GUI prompt: the
    // GitBolt binary re-invoked in askpass mode (see main.cpp, keyed
    // on GITBOLT_ASKPASS_MODE — ssh passes no flags, so an env marker
    // is the only reliable switch). SSH_ASKPASS_REQUIRE=force makes
    // OpenSSH ≥ 8.4 use the askpass even when a TTY exists.
    const QString self = QCoreApplication::applicationFilePath();
    if (!self.isEmpty()) {
        env.insert(QStringLiteral("GIT_ASKPASS"), self);
        env.insert(QStringLiteral("SSH_ASKPASS"), self);
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"),
                   QStringLiteral("force"));
        env.insert(QStringLiteral("GITBOLT_ASKPASS_MODE"),
                   QStringLiteral("1"));
    }
    process.setProcessEnvironment(env);
}

int GitProcess::defaultTimeoutMs() {
    bool ok = false;
    const int fromEnv = qEnvironmentVariableIntValue("GITBOLT_GIT_TIMEOUT_MS", &ok);
    return ok && fromEnv > 0 ? fromEnv : 30000;
}

Result<ProcessOutput> GitProcess::run(const std::vector<std::string>& args,
                                      std::optional<int> timeout) const {
    const int timeoutMs = timeout.value_or(defaultTimeoutMs());
    if (!cancelFlags_.empty())
        return runCancellable(args, timeoutMs);

    QProcess process;
    process.setWorkingDirectory(QString::fromStdString(workDir_));
    applyEnvironment(process);

    QStringList qargs;
    for (const auto& arg : args)
        qargs.append(QString::fromStdString(arg));

    // Wall-clock timing the run so the command log can show per-
    // command latency. Cheap (no allocations after start()).
    QElapsedTimer timer;
    timer.start();

    process.start(QString::fromStdString(gitPath_), qargs);

    if (!process.waitForStarted(5000)) {
        // Still log the failed-to-start case with exit code -1 so
        // it shows up in the command log; users debugging "why
        // didn't this run?" want to see attempts as well as
        // successes.
        GitProcessLog::instance().emitCommand(
            QString::fromStdString(workDir_), qargs, -1,
            timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed, "Failed to start git process");
    }

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        GitProcessLog::instance().emitCommand(
            QString::fromStdString(workDir_), qargs, -1,
            timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed, "Git process timed out");
    }

    ProcessOutput output;
    output.exitCode = process.exitCode();
    output.stdoutData = process.readAllStandardOutput().toStdString();
    output.stderrData = process.readAllStandardError().toStdString();

    GitProcessLog::instance().emitCommand(
        QString::fromStdString(workDir_), qargs, output.exitCode,
        timer.elapsed());

    return output;
}

Result<ProcessOutput> GitProcess::runWithInput(
    const std::vector<std::string>& args,
    const std::string& stdinData,
    std::optional<int> timeout) const
{
    const int timeoutMs = timeout.value_or(defaultTimeoutMs());
    QProcess process;
    process.setWorkingDirectory(QString::fromStdString(workDir_));
    applyEnvironment(process);

    QStringList qargs;
    for (const auto& arg : args)
        qargs.append(QString::fromStdString(arg));

    QElapsedTimer timer;
    timer.start();

    process.start(QString::fromStdString(gitPath_), qargs);
    if (!process.waitForStarted(5000)) {
        GitProcessLog::instance().emitCommand(
            QString::fromStdString(workDir_), qargs, -1,
            timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed,
                        "Failed to start git process");
    }

    // Feed the payload and close stdin so git sees EOF — commands
    // like `git apply -` block until the write channel closes.
    process.write(stdinData.data(),
                  static_cast<qint64>(stdinData.size()));
    process.closeWriteChannel();

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        GitProcessLog::instance().emitCommand(
            QString::fromStdString(workDir_), qargs, -1,
            timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed,
                        "Git process timed out");
    }

    ProcessOutput output;
    output.exitCode = process.exitCode();
    output.stdoutData = process.readAllStandardOutput().toStdString();
    output.stderrData = process.readAllStandardError().toStdString();

    GitProcessLog::instance().emitCommand(
        QString::fromStdString(workDir_), qargs, output.exitCode,
        timer.elapsed());

    return output;
}

Result<std::vector<std::string>> GitProcess::logOneline(const std::string& range, int maxCount) const {
    std::vector<std::string> args = {"log", "--oneline", "--format=%H %s"};
    if (maxCount > 0) { args.emplace_back("-n"); args.push_back(std::to_string(maxCount)); }
    if (!range.empty()) args.push_back(range);

    auto result = run(args);
    if (!result) return result.error();

    std::vector<std::string> lines;
    std::istringstream stream(result->stdoutData);
    std::string line;
    while (std::getline(stream, line))
        if (!line.empty()) lines.push_back(std::move(line));
    return lines;
}

Result<std::string> GitProcess::diffRaw(const std::string& from, const std::string& to) const {
    auto result = run({"diff", from, to});
    if (!result) return result.error();
    return result->stdoutData;
}

Result<std::string> GitProcess::showFile(const std::string& revision, const std::string& path) const {
    auto result = run({"show", revision + ":" + path});
    if (!result) return result.error();
    return result->stdoutData;
}

Result<ProcessOutput> GitProcess::push(const std::string& remote, const std::string& branch,
                                       bool force, bool setUpstream) const {
    // git is strict about empty refspecs: `git push origin ""` errors
    // with "fatal: invalid refspec ''" instead of doing the sensible
    // thing (push the current branch to its upstream). Drop empty
    // arguments so the toolbar's "Push" with no specific branch hits
    // the bare `git push origin` form, which honors push.default.
    // reserve() instead of `= {"push"}`: growing a vector built from a
    // one-element initializer list trips GCC 13's -Warray-bounds false
    // positive inside <basic_string.h> (same in pull/fetch below).
    std::vector<std::string> args;
    args.reserve(5);
    args.emplace_back("push");
    if (force) args.emplace_back("--force-with-lease");
    if (setUpstream) args.emplace_back("--set-upstream");
    if (!remote.empty()) args.push_back(remote);
    if (!branch.empty()) args.push_back(branch);
    return run(args, 120000);
}

Result<ProcessOutput> GitProcess::pull(const std::string& remote, const std::string& branch) const {
    // Same empty-arg avoidance as push — `git pull origin ""` errors
    // out, but bare `git pull origin` (or just `git pull`) honors the
    // tracking branch.
    std::vector<std::string> args;
    args.reserve(3);
    args.emplace_back("pull");
    if (!remote.empty()) args.push_back(remote);
    if (!branch.empty()) args.push_back(branch);
    return run(args, 120000);
}

Result<ProcessOutput> GitProcess::fetch(const std::string& remote, bool prune) const {
    std::vector<std::string> args;
    args.reserve(3);
    args.emplace_back("fetch");
    if (!remote.empty()) args.push_back(remote);
    if (prune) args.emplace_back("--prune");
    return run(args, 120000);
}

// ---------------------------------------------------------------------------
// Clone, and run() with a cancel flag
// ---------------------------------------------------------------------------
namespace {

constexpr int kStartTimeoutMs = 5000;
// How long a cancel can go unnoticed: the output loop wakes at least
// this often to check the flag.
constexpr int kPollMs = 50;
// Per step of stopping the process tree; bounded so a wedged child
// can never hang the worker (and through the thread pool, app exit).
constexpr int kTerminateWaitMs = 2000;

// The process tree of one `git clone` (or of a cancellable run(): a
// fetch, pull or push). git does the transfer in children —
// git-remote-https, ssh, index-pack, upload-pack for a local URL,
// GitBolt itself in askpass mode — so killing only git on cancel
// orphans them: they run on, and on Windows keep files inside the
// destination open, so it can't be removed. ProcessTree puts git
// in a group that one call stops as a whole:
//
//   Unix:    setsid() in the child makes git the leader of a new
//            process group; terminate() signals the group.
//   Windows: git is created inside a job object (the attribute is
//            applied by CreateProcess itself, so not even a child
//            spawned in git's first microseconds escapes);
//            terminate() ends the job, and KILL_ON_JOB_CLOSE kills
//            whatever is left if GitBolt itself dies mid-clone.
//
// Only while git runs, though. Once git has exited on its own,
// release() lets the tree go, and whatever git or a helper started
// to outlive it keeps running: a browser that Git Credential Manager
// opened for a sign-in, an fsmonitor or credential-cache daemon.
class ProcessTree {
public:
    ProcessTree();
    ~ProcessTree();
    ProcessTree(const ProcessTree&) = delete;
    ProcessTree& operator=(const ProcessTree&) = delete;

    /// Install the grouping hook on `process`, before start().
    void prepare(QProcess& process);
    /// After a failed start: remove the hook so the caller can retry
    /// plain. False when there was nothing to remove.
    bool dropHook(QProcess& process);
    /// Stop git and all of its descendants; returns once git exited.
    void terminate(QProcess& process);
    /// git exited on its own: let go of the tree, stopping nothing
    /// that is still in it. Every exit but a cancel ends with this.
    void release();

private:
#if defined(Q_OS_WIN)
    HANDLE job_ = nullptr;
    std::vector<unsigned char> attrBuffer_;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs_ = nullptr;
    STARTUPINFOEXW startupEx_{};
#endif
};

#if defined(Q_OS_WIN)

ProcessTree::ProcessTree()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return;
    // Kill-on-close, and deliberately no BREAKAWAY_OK: the msys2
    // runtime under Git for Windows' sh.exe and ssh.exe adds
    // CREATE_BREAKAWAY_FROM_JOB to every process it starts whenever
    // the job allows breakaway, which would let ssh's askpass prompt,
    // a core.sshCommand or a hook's children escape Cancel. With
    // breakaway not allowed msys2 doesn't ask, and everything git
    // starts stays in the job.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SIZE_T size = 0;
    // The sizing call fails by design and reports the size it needs.
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    attrBuffer_.resize(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer_.data());
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                 static_cast<DWORD>(sizeof(limits)))
        || size == 0
        || !InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
        CloseHandle(job);
        return;
    }
    // The attribute list keeps a pointer to the handle: point it at
    // the member, which lives as long as the list.
    job_ = job;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                   &job_, sizeof(job_), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(job_);
        job_ = nullptr;
        return;
    }
    attrs_ = attrs;
}

ProcessTree::~ProcessTree()
{
    if (attrs_)
        DeleteProcThreadAttributeList(attrs_);
    if (job_)
        CloseHandle(job_);
}

void ProcessTree::prepare(QProcess& process)
{
    if (!attrs_)
        return;
    process.setCreateProcessArgumentsModifier(
        [this](QProcess::CreateProcessArguments* args) {
            // Qt's STARTUPINFOW carries the redirected std handles, so
            // extend a copy of it rather than replacing it. The flags
            // are OR'ed: Qt's CREATE_NO_WINDOW (set because GitBolt has
            // no console) stays, and the hidden console it creates is
            // inherited by everything git starts — no console windows.
            startupEx_ = {};
            startupEx_.StartupInfo = *args->startupInfo;
            startupEx_.StartupInfo.cb = static_cast<DWORD>(sizeof(startupEx_));
            startupEx_.lpAttributeList = attrs_;
            args->startupInfo = &startupEx_.StartupInfo;
            args->flags |= EXTENDED_STARTUPINFO_PRESENT;
        });
}

bool ProcessTree::dropHook(QProcess& process)
{
    if (!attrs_)
        return false;
    process.setCreateProcessArgumentsModifier({});
    DeleteProcThreadAttributeList(attrs_);
    attrs_ = nullptr;
    CloseHandle(job_);
    job_ = nullptr;
    return true;
}

void ProcessTree::terminate(QProcess& process)
{
    if (job_)
        TerminateJobObject(job_, 1);
    else
        process.kill();     // no job: git only; restoreDestination retries
    if (!process.waitForFinished(kTerminateWaitMs)) {
        process.kill();
        process.waitForFinished(kTerminateWaitMs);
    }
}

void ProcessTree::release()
{
    if (!job_)
        return;
    // Clear every limit, kill-on-close with it, before closing our
    // handle (the only one): otherwise the close would end whatever
    // is still in the job. The job itself lives on until its last
    // process exits.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION none{};
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &none,
                            static_cast<DWORD>(sizeof(none)));
    CloseHandle(job_);
    job_ = nullptr;
}

#else

ProcessTree::ProcessTree() = default;
ProcessTree::~ProcessTree() = default;

void ProcessTree::prepare(QProcess& process)
{
    // Runs in the child between fork and exec: async-signal-safe
    // calls only. A new session also has no controlling terminal, so
    // for a GitBolt started from a shell a prompt that would read
    // /dev/tty falls through to the askpass (as in a Finder launch)
    // instead of stopping the background group with SIGTTIN.
    process.setChildProcessModifier([] { ::setsid(); });
}

bool ProcessTree::dropHook(QProcess&)
{
    return false;
}

void ProcessTree::terminate(QProcess& process)
{
    const auto group = static_cast<pid_t>(process.processId());
    if (group <= 0) {
        process.kill();
        process.waitForFinished(kTerminateWaitMs);
        return;
    }
    // SIGTERM first: git clone's handler deletes the half-written
    // destination itself, and ssh closes its connection. SIGKILL then
    // sweeps the group for anything that ignored SIGTERM or outlived
    // git (ESRCH once the group is empty).
    ::kill(-group, SIGTERM);
    process.waitForFinished(kTerminateWaitMs);
    ::kill(-group, SIGKILL);
    process.waitForFinished(kTerminateWaitMs);
}

void ProcessTree::release()
{
    // Nothing to let go of: the group is only ever signalled on cancel.
}

#endif

// Any entry makes a directory non-empty for git — .DS_Store and other
// hidden files included — and so it must for us.
constexpr QDir::Filters kAnyEntry =
    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

// Delete what `dir` holds, but not `dir` itself — as git's own
// cleanup does for a destination that already existed — so the
// folder the user picked keeps its permissions, ACLs and Finder tags.
// Entries go the way QDir::removeRecursively() deletes them: a
// symlink is removed, not followed, and a read-only file is made
// writable first (Windows refuses to delete it otherwise).
void removeContents(const QString& dir)
{
    QDirIterator it(dir, kAnyEntry);
    while (it.hasNext()) {
        const QFileInfo entry = it.nextFileInfo();
        const QString entryPath = entry.filePath();
        if (entry.isDir() && !entry.isSymLink()) {
            QDir(entryPath).removeRecursively();
        } else if (!QFile::remove(entryPath)) {
            QFile::setPermissions(entryPath,
                                  QFile::permissions(entryPath) | QFile::WriteUser);
            QFile::remove(entryPath);
        }
    }
}

// Put the destination back the way it was before the clone: gone, or
// an empty directory if it already existed. git's own exit/signal
// handler has usually done this already, but SIGKILL and
// TerminateJobObject give it no chance, and it gives up at the first
// entry it can't delete. On Windows a file can't be deleted while any
// process still has it open — a child that is still dying, a virus
// scanner — so retry with backoff, bounded (~3 s in all) so a stuck
// handle leaves a directory behind instead of wedging the worker.
void restoreDestination(const QString& dest, bool existedBefore)
{
    unsigned long delayMs = 50;
    for (int attempt = 0;; ++attempt) {
        bool restored = false;
        if (existedBefore) {
            QDir().mkpath(dest);    // a no-op unless something removed it
            removeContents(dest);
            restored = QDir(dest).isEmpty(kAnyEntry);
        } else {
            QDir(dest).removeRecursively();
            restored = !QFileInfo::exists(dest);
        }
        if (restored || attempt == 7)
            return;
        QThread::msleep(delayMs);
        delayMs = delayMs < 400 ? delayMs * 2 : 800;
    }
}

// The clone itself completed: HEAD names a commit, which it does only
// once the fetch is done and its refs are written. From there on git
// keeps the repository whatever fails next — the checkout, or a
// post-checkout hook after it. Asked of the git that cloned, not read
// from its messages or opened with libgit2, so the answer holds
// whatever language git speaks and whatever format (reftable,
// SHA-256) it wrote. --git-dir, because a destination without a .git
// of its own would otherwise have git search upwards and answer for
// an enclosing repository.
bool cloneCompleted(const QString& dest)
{
    const QString gitDir = QDir(dest).filePath(QStringLiteral(".git"));
    if (!QFileInfo(gitDir).isDir())
        return false;
    const auto out = GitProcess(dest.toStdString())
                         .run({"--git-dir=" + gitDir.toStdString(), "rev-parse", "--verify",
                               "--quiet", "HEAD^{commit}"});
    return out.ok() && out->success();
}

} // namespace

bool GitProcess::cancelled() const
{
    return std::any_of(cancelFlags_.begin(), cancelFlags_.end(),
                       [](const auto& flag) { return flag->load(); });
}

// run() for a GitProcess with a cancel flag: git in a ProcessTree, and
// a poll loop instead of one long waitForFinished(), so a cancel is
// seen within kPollMs and stops the whole tree — including an ssh or
// git-remote-https waiting on a network that stopped answering, which
// would otherwise hold its caller until the timeout.
Result<ProcessOutput> GitProcess::runCancellable(const std::vector<std::string>& args,
                                                 int timeoutMs) const
{
    const QString workDir = QString::fromStdString(workDir_);
    QStringList qargs;
    for (const auto& arg : args)
        qargs.append(QString::fromStdString(arg));
    if (cancelled())
        return GitError(GitErrorCode::User, "Cancelled");

    // Declared before the process so it outlives it, as in clone().
    ProcessTree tree;
    QProcess process;
    process.setWorkingDirectory(workDir);
    applyEnvironment(process);
    tree.prepare(process);

    QElapsedTimer timer;
    timer.start();
    const QString gitPath = QString::fromStdString(gitPath_);
    process.start(gitPath, qargs);
    bool started = process.waitForStarted(kStartTimeoutMs);
    if (!started && tree.dropHook(process)) {
        process.start(gitPath, qargs);
        started = process.waitForStarted(kStartTimeoutMs);
    }
    if (!started) {
        GitProcessLog::instance().emitCommand(workDir, qargs, -1, timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed, "Failed to start git process");
    }

    // Each wait also moves git's output into QProcess's buffers, so
    // its pipes can't fill up and stall it between polls.
    while (!process.waitForFinished(kPollMs) && process.state() != QProcess::NotRunning) {
        const bool stop = cancelled();
        if (stop || (timeoutMs >= 0 && timer.elapsed() >= timeoutMs)) {
            tree.terminate(process);
            GitProcessLog::instance().emitCommand(workDir, qargs, -1, timer.elapsed());
            if (stop)
                return GitError(GitErrorCode::User, "Cancelled");
            return GitError(GitErrorCode::ProcessFailed, "Git process timed out");
        }
    }
    tree.release();

    ProcessOutput output;
    output.exitCode = process.exitCode();
    output.stdoutData = process.readAllStandardOutput().toStdString();
    output.stderrData = process.readAllStandardError().toStdString();
    GitProcessLog::instance().emitCommand(workDir, qargs, output.exitCode,
                                          timer.elapsed());
    return output;
}

// The command log is shown in the UI, and a URL can carry a password
// or a token. Only the userinfo of a "scheme://" URL is touched: an
// scp-style "git@host:path" has none to hide.
std::string GitProcess::redactUrl(const std::string& url)
{
    const QString s = QString::fromStdString(url);
    const qsizetype schemeEnd = s.indexOf(QStringLiteral("://"));
    if (schemeEnd < 0)
        return url;
    // The authority runs up to the path, query or fragment, and the
    // userinfo up to its last '@' — so a password with a raw '@' in
    // it is masked whole, and an '@' in the path is left alone.
    const qsizetype authority = schemeEnd + 3;
    qsizetype authorityEnd = authority;
    while (authorityEnd < s.size() && !QStringLiteral("/?#").contains(s[authorityEnd]))
        ++authorityEnd;
    const qsizetype at = s.lastIndexOf(QLatin1Char('@'), authorityEnd - 1);
    if (at < authority)
        return url;     // no userinfo; a ':' is just the port
    const qsizetype colon = s.indexOf(QLatin1Char(':'), authority);
    if (colon >= 0 && colon < at)       // user:secret, the user kept
        return (s.left(colon + 1) + QStringLiteral("***") + s.mid(at)).toStdString();
    const QString scheme = s.left(schemeEnd).toLower();
    if (scheme == QStringLiteral("ssh") || scheme == QStringLiteral("git+ssh")
        || scheme == QStringLiteral("ssh+git"))
        return url;     // an ssh login name, like scp-style's "git@"
    // Anything else with no ':' is a token used as the user name
    // (https://<token>@github.com/…): all of it goes.
    return (s.left(authority) + QStringLiteral("***") + s.mid(at)).toStdString();
}

Result<void> GitProcess::clone(const std::string& url, const std::string& path,
                               const CloneProgressCallback& onProgress,
                               const std::shared_ptr<std::atomic<bool>>& cancelFlag)
{
    // Absolute, so the result can't depend on the working directory
    // git inherits.
    const QString dest =
        QFileInfo(QString::fromStdString(path)).absoluteFilePath();
    const QFileInfo destInfo(dest);
    const bool existedBefore = destInfo.exists();
    // git refuses these too, but checking here is what makes the
    // cleanup safe: it only ever removes what this clone created.
    if (existedBefore && (!destInfo.isDir() || !QDir(dest).isEmpty(kAnyEntry)))
        return GitError(GitErrorCode::Exists,
                        "destination path '" + path +
                        "' already exists and is not an empty directory");

    const QStringList args = {QStringLiteral("clone"), QStringLiteral("--progress"),
                              QStringLiteral("--"), QString::fromStdString(url), dest};
    QStringList logArgs = args;
    logArgs[3] = QString::fromStdString(redactUrl(url));
    const QString logDir = QDir::currentPath();
    const QString gitPath = QString::fromStdString(findGitExecutable());

    // Declared before the process so it outlives it: the job handle
    // must stay open while git runs.
    ProcessTree tree;
    QProcess process;
    applyEnvironment(process);
    // CloneProgressParser matches git's English progress titles and
    // its "Clone succeeded, but checkout failed" warning, so keep
    // gettext from translating them (or the errors, which were
    // English under libgit2 too). LANGUAGE outranks LC_ALL / LANG for
    // messages and leaves the character set alone.
    QProcessEnvironment env = process.processEnvironment();
    env.insert(QStringLiteral("LANGUAGE"), QStringLiteral("en"));
    process.setProcessEnvironment(env);
    // Clone writes everything to stderr; merged, one channel carries it
    // all and no unread pipe can fill up and stall git.
    process.setProcessChannelMode(QProcess::MergedChannels);
    tree.prepare(process);

    QElapsedTimer timer;
    timer.start();
    process.start(gitPath, args);
    bool started = process.waitForStarted(kStartTimeoutMs);
    if (!started && tree.dropHook(process)) {
        // CreateProcess can reject the job attribute (a parent job
        // that forbids nesting): clone without the group rather than
        // not at all.
        process.start(gitPath, args);
        started = process.waitForStarted(kStartTimeoutMs);
    }
    if (!started) {
        GitProcessLog::instance().emitCommand(logDir, logArgs, -1, timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed,
                        "Failed to start git: " + process.errorString().toStdString());
    }

    CloneProgressParser parser;
    auto deliver = [&onProgress](const std::vector<CloneProgress>& updates) {
        if (onProgress)
            for (const auto& u : updates)
                onProgress(u);
    };
    auto drain = [&]() {
        const QByteArray chunk = process.readAll();
        if (!chunk.isEmpty())
            deliver(parser.feed(std::string_view(
                chunk.constData(), static_cast<std::size_t>(chunk.size()))));
    };

    bool cancelled = false;
    while (process.state() != QProcess::NotRunning) {
        if (cancelFlag && cancelFlag->load()) {
            cancelled = true;
            break;
        }
        process.waitForReadyRead(kPollMs);
        drain();
    }

    if (cancelled) {
        tree.terminate(process);
        GitProcessLog::instance().emitCommand(logDir, logArgs, -1, timer.elapsed());
        restoreDestination(dest, existedBefore);
        return GitError(GitErrorCode::User, "Clone cancelled");
    }

    // git exited on its own, so whatever it left running was meant to
    // outlive it.
    tree.release();
    drain();
    deliver(parser.finish());
    const int exitCode =
        process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    GitProcessLog::instance().emitCommand(logDir, logArgs, exitCode, timer.elapsed());
    if (exitCode == 0)
        return Result<void>::success();

    std::string message = parser.messages();
    if (message.empty())
        message = exitCode < 0 ? std::string("git clone crashed")
                               : "git clone exited with code " + std::to_string(exitCode);

    // Cloned, then the checkout failed, or a post-checkout hook did
    // (core.hooksPath, init.templateDir). git keeps that repository on
    // purpose — the download is complete — so it isn't ours to remove.
    if (cloneCompleted(dest)) {
        // A failed checkout comes with git's warning and its advice on
        // finishing the checkout. A failed hook with nothing of git's:
        // the files are checked out, git exits with the hook's status,
        // and what the hook printed is all there is to show.
        const bool hookFailed = exitCode > 0 && !parser.checkoutFailed();
        return GitError(GitErrorCode::CheckoutFailed,
                        "The repository was cloned to '"
                        + QDir::toNativeSeparators(dest).toStdString() + "', but "
                        + (hookFailed ? "a post-checkout hook failed" : "its checkout failed")
                        + ":\n" + message);
    }

    restoreDestination(dest, existedBefore);
    return GitError(GitErrorCode::ProcessFailed, message);
}

Result<ProcessOutput> GitProcess::interactiveRebase(
    const std::string& onto, const std::string& todo,
    const std::map<std::string, std::string>& newMessages,
    const std::vector<std::string>& messages) const {
    std::vector<std::string> allMessages = messages;
    for (const auto& [hash, message] : newMessages)
        allMessages.push_back(message);
    const char commentChar = commentCharFor(allMessages);
    if (commentChar == '\0')
        return GitError(GitErrorCode::ProcessFailed,
                        "Lines of these commit messages start with every character git can "
                        "take for a comment (#;@!$%^&|:), so git would cut some of them out.");

    // git writes the todo list it would run to a file and hands that
    // file to GIT_SEQUENCE_EDITOR, a command it runs through its shell
    // (sh, also with Git for Windows) with the file's path appended.
    // Ours copies `todo` over it. This used to put the todo text
    // itself in GIT_SEQUENCE_EDITOR, which git then ran as a command
    // ("pick: command not found"), so every rebase failed.
    const auto writeFile = [](QFile& file, const std::string& text) {
        const auto size = static_cast<qint64>(text.size());
        return file.write(text.data(), size) == size && file.flush();
    };
    QTemporaryFile todoFile(QDir::tempPath() + QStringLiteral("/gitbolt-rebase-XXXXXX"));
    if (!todoFile.open() || !writeFile(todoFile, todo))
        return GitError(GitErrorCode::ProcessFailed,
                        "Could not write the rebase plan: " + todoFile.errorString().toStdString());
    todoFile.close();
    const auto shellQuoted = [](const QString& path) {
        QString quoted = QDir::fromNativeSeparators(path);
        quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QStringLiteral("'%1'").arg(quoted);
    };
    // The new messages, and the comment character, go into the rebase's
    // own state, for kEditor and rebaseCommentConfig() to find: the
    // sequence editor runs once git has set that up, and before any
    // commit is made.
    QTemporaryDir messageDir(QDir::tempPath() + QStringLiteral("/gitbolt-messages-XXXXXX"));
    if (!messageDir.isValid())
        return GitError(GitErrorCode::ProcessFailed,
                        "Could not write the rebase plan: " + messageDir.errorString().toStdString());
    std::map<QString, std::string> stateFiles = {{kCommentChar, std::string(1, commentChar)}};
    for (const auto& [hash, message] : newMessages)
        stateFiles[QString::fromStdString(hash)] = message;
    for (const auto& [name, text] : stateFiles) {
        QFile file(messageDir.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || !writeFile(file, text))
            return GitError(GitErrorCode::ProcessFailed,
                            "Could not write the rebase plan: " + file.errorString().toStdString());
    }
    const QString sequenceEditor = QStringLiteral(
        "cp %1 \"$1\" && cp -R %2 \"$(git rev-parse --git-path rebase-merge)/%3\" && :")
        .arg(shellQuoted(todoFile.fileName()), shellQuoted(messageDir.path()), kRewordMessages);

    QProcess process;
    process.setWorkingDirectory(QString::fromStdString(workDir_));
    applyEnvironment(process);
    QProcessEnvironment env = process.processEnvironment();
    env.insert(QStringLiteral("GIT_SEQUENCE_EDITOR"), sequenceEditor);
    process.setProcessEnvironment(env);

    // The git commands the rebase runs (`git commit` for a reword or a
    // squash) inherit the -c.
    const QStringList args = {QStringLiteral("-c"),
                              QStringLiteral("core.commentChar=") + QLatin1Char(commentChar),
                              QStringLiteral("rebase"), QStringLiteral("-i"),
                              QString::fromStdString(onto)};
    const QString workDir = QString::fromStdString(workDir_);
    QElapsedTimer timer;
    timer.start();
    process.start(QString::fromStdString(gitPath_), args);
    if (!process.waitForStarted(5000)) {
        GitProcessLog::instance().emitCommand(workDir, args, -1, timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed, "Failed to start rebase");
    }
    if (!process.waitForFinished(kRebaseTimeoutMs)) {
        process.kill();
        GitProcessLog::instance().emitCommand(workDir, args, -1, timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed, "Rebase timed out");
    }

    ProcessOutput output;
    output.exitCode = process.exitCode();
    output.stdoutData = process.readAllStandardOutput().toStdString();
    output.stderrData = process.readAllStandardError().toStdString();
    GitProcessLog::instance().emitCommand(workDir, args, output.exitCode, timer.elapsed());
    return output;
}

std::vector<std::string> GitProcess::rebaseCommentConfig() const {
    const auto path = run({"rev-parse", "--git-path",
                           QStringLiteral("rebase-merge/%1/%2")
                               .arg(kRewordMessages, kCommentChar).toStdString()});
    if (!path || !path->success())
        return {};
    QFile file(QDir(QString::fromStdString(workDir_))
                   .absoluteFilePath(QString::fromStdString(path->stdoutData).trimmed()));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray commentChar = file.read(2);
    if (commentChar.size() != 1)
        return {};
    return {"-c", "core.commentChar=" + commentChar.toStdString()};
}

Result<ProcessOutput> GitProcess::rebaseContinue() const {
    std::vector<std::string> args = rebaseCommentConfig();
    args.insert(args.end(), {"rebase", "--continue"});
    return run(args, kRebaseTimeoutMs);
}

Result<ProcessOutput> GitProcess::rebaseAbort() const {
    return run({"rebase", "--abort"}, kRebaseTimeoutMs);
}

Result<ProcessOutput> GitProcess::rebaseSkip() const {
    std::vector<std::string> args = rebaseCommentConfig();
    args.insert(args.end(), {"rebase", "--skip"});
    return run(args, kRebaseTimeoutMs);
}

Result<ProcessOutput> GitProcess::gitFlowInit() const { return run({"flow", "init", "-d"}); }
Result<ProcessOutput> GitProcess::gitFlowFeatureStart(const std::string& n) const { return run({"flow", "feature", "start", n}); }
Result<ProcessOutput> GitProcess::gitFlowFeatureFinish(const std::string& n) const { return run({"flow", "feature", "finish", n}); }
Result<ProcessOutput> GitProcess::gitFlowReleaseStart(const std::string& v) const { return run({"flow", "release", "start", v}); }
Result<ProcessOutput> GitProcess::gitFlowReleaseFinish(const std::string& v) const { return run({"flow", "release", "finish", v}); }
Result<ProcessOutput> GitProcess::gitFlowHotfixStart(const std::string& v) const { return run({"flow", "hotfix", "start", v}); }
Result<ProcessOutput> GitProcess::gitFlowHotfixFinish(const std::string& v) const { return run({"flow", "hotfix", "finish", v}); }

Result<ProcessOutput> GitProcess::gc() const { return run({"gc", "--auto"}, 300000); }
Result<ProcessOutput> GitProcess::prune() const { return run({"prune"}, 120000); }
Result<ProcessOutput> GitProcess::fsck() const { return run({"fsck", "--full"}, 300000); }

std::string GitProcess::findGitExecutable() {
    QString path = QStandardPaths::findExecutable("git");
    if (!path.isEmpty()) return path.toStdString();
    for (const char* p : {"/usr/bin/git", "/usr/local/bin/git", "/opt/homebrew/bin/git"})
        if (QFile::exists(p)) return p;
    return "git";
}

} // namespace gitbolt::git
