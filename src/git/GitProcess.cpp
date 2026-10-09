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
#include <QThread>
#include <sstream>

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

Result<ProcessOutput> GitProcess::run(const std::vector<std::string>& args, int timeoutMs) const {
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
    int timeoutMs) const
{
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
// Clone
// ---------------------------------------------------------------------------
namespace {

constexpr int kStartTimeoutMs = 5000;
// How long a cancel can go unnoticed: the output loop wakes at least
// this often to check the flag.
constexpr int kPollMs = 50;
// Per step of stopping the process tree; bounded so a wedged child
// can never hang the worker (and through the thread pool, app exit).
constexpr int kTerminateWaitMs = 2000;

// The process tree of one `git clone`. git does the transfer in
// children — git-remote-https, ssh, index-pack, upload-pack for a
// local URL, GitBolt itself in askpass mode — so killing only git on
// cancel orphans them: they run on, and on Windows keep files inside
// the destination open, so it can't be removed. ProcessTree puts git
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

} // namespace

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

    // Fetched, then the checkout failed. git keeps that repository on
    // purpose (the download is complete, and the advice it printed
    // says how to finish the checkout), so it isn't ours to remove.
    if (parser.checkoutFailed() && QFileInfo(QDir(dest).filePath(QStringLiteral(".git"))).isDir())
        return GitError(GitErrorCode::CheckoutFailed,
                        "The repository was cloned to '"
                        + QDir::toNativeSeparators(dest).toStdString()
                        + "', but its checkout failed:\n" + message);

    restoreDestination(dest, existedBefore);
    return GitError(GitErrorCode::ProcessFailed, message);
}

Result<ProcessOutput> GitProcess::interactiveRebase(const std::string& onto, const std::string& editorScript) const {
    QProcess process;
    process.setWorkingDirectory(QString::fromStdString(workDir_));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("GIT_SEQUENCE_EDITOR", QString::fromStdString(editorScript));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process.setProcessEnvironment(env);
    process.start(QString::fromStdString(gitPath_), {"rebase", "-i", QString::fromStdString(onto)});

    if (!process.waitForStarted(5000))
        return GitError(GitErrorCode::ProcessFailed, "Failed to start rebase");
    if (!process.waitForFinished(300000)) {
        process.kill();
        return GitError(GitErrorCode::ProcessFailed, "Rebase timed out");
    }

    ProcessOutput output;
    output.exitCode = process.exitCode();
    output.stdoutData = process.readAllStandardOutput().toStdString();
    output.stderrData = process.readAllStandardError().toStdString();
    return output;
}

Result<ProcessOutput> GitProcess::rebaseContinue() const { return run({"rebase", "--continue"}); }
Result<ProcessOutput> GitProcess::rebaseAbort() const { return run({"rebase", "--abort"}); }
Result<ProcessOutput> GitProcess::rebaseSkip() const { return run({"rebase", "--skip"}); }

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
