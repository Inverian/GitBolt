#include "git/GitProcess.h"
#include "git/Askpass.h"
#include "git/GitProcessLog.h"
#include "util/LoginPath.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QHash>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>
#include <QThreadPool>
#include <optional>
#include <algorithm>
#include <atomic>
#include <mutex>
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

// Where commit() notes the stops of a cherry-pick or revert of several
// commits that it concluded: "<stopped commit> <commit made>" a line. In
// git's sequencer folder, so that it lives exactly as long as the
// sequence does: git deletes the folder when the sequence finishes or is
// aborted (or quit).
const QString kConcludedStops = QStringLiteral("sequencer/gitbolt-concluded");

// Every step of a rebase may replay the rest of the plan in one go, and
// each reword in it runs the commit hooks. 30 s, run()'s default, cut
// a Continue off mid-plan, git killed and its index.lock left behind.
constexpr int kRebaseTimeoutMs = 300000;

// A commit runs the repository's hooks, and a pre-commit hook may lint
// or test the whole tree: as long as a rebase step, not run()'s 30 s.
constexpr int kCommitTimeoutMs = 300000;

// Where git is when the PATH has none: the places git's packages put it
// (Homebrew on Apple silicon has one of its own). After the PATH; none
// on Windows, where these would be taken on the current drive.
const std::vector<std::string>& gitFallbacks() {
#ifdef Q_OS_WIN
    static const std::vector<std::string> none;
    return none;
#else
    static const std::vector<std::string> places{"/usr/bin/git", "/usr/local/bin/git",
                                                 "/opt/homebrew/bin/git"};
    return places;
#endif
}

// What a git command fails with when there is no git to run: never a
// bare "git" instead, which Windows looks for in the current folder
// (GitBolt's, and after Explorer's "Open in GitBolt" a repository)
// before PATH.
constexpr char kNoGit[] =
    "git was not found: install git, or add the folder it is in to your PATH";

// findGitExecutable()'s answers, by the PATH they were found on: a test
// that puts a git of its own first gets that one, and the user's is
// looked up once. An answer counts while its file is there.
struct GitByPath {
    std::mutex mutex;
    QHash<QString, QString> found;
};

GitByPath& gitByPath() {
    static GitByPath instance;
    return instance;
}

QString gitOnPath(const QString& pathList) {
    GitByPath& cache = gitByPath();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto known = cache.found.constFind(pathList);
        if (known != cache.found.constEnd() && QFileInfo(*known).isExecutable())
            return *known;
    }
    const QString program = QString::fromStdString(
        GitProcess::findGitOnPath(pathList.toStdString(), gitFallbacks()));
    if (!program.isEmpty()) {
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.found.insert(pathList, program);
    }
    return program;
}

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
    : workDir_(workingDirectory) {}

const std::vector<std::string>& GitProcess::repositoryEnvVars() {
    // `git rev-parse --local-env-vars`, less GIT_CONFIG_PARAMETERS and
    // GIT_CONFIG_COUNT (`git -c` settings: how git is to behave, not
    // where). GIT_CONFIG goes too: it is the one file a plain `git
    // config` then reads, so the fetch-prune check read nothing of the
    // repository's and let a fetch delete local tags.
    static const std::vector<std::string> names{
        "GIT_DIR", "GIT_WORK_TREE", "GIT_IMPLICIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_CONFIG",
        "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR",
        "GIT_GRAFT_FILE", "GIT_NO_REPLACE_OBJECTS", "GIT_REPLACE_REF_BASE", "GIT_PREFIX",
        "GIT_INTERNAL_SUPER_PREFIX", "GIT_SHALLOW_FILE"};
    return names;
}

void GitProcess::applyEnvironment(QProcess& process, bool allowPrompts) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Where the repository is comes from the working directory (and
    // GIT_DIR for a bare one, runImpl), never from GitBolt's own
    // environment: started from a shell that exports GIT_DIR (a bare
    // dotfiles setup), from a git alias or a hook, every command went
    // to that repository, while libgit2, which ignores these, showed
    // the one that is open.
    for (const auto& name : repositoryEnvVars())
        env.remove(QString::fromStdString(name));
    // The login shell's PATH ahead of the session's (util/LoginPath.h):
    // launched from the Dock, GitBolt has launchd's, and a hook that
    // runs node, npx or anything else from Homebrew, nvm or volta
    // turned every commit down ("node: command not found").
    env.insert(QStringLiteral("PATH"),
               util::pathForChildren(env.value(QStringLiteral("PATH"))));
#ifdef Q_OS_WIN
    // A program git, a hook or a credential helper starts by name comes
    // from PATH, never first from the current folder, which for git is
    // the work tree: CreateProcess and cmd.exe look there unless this is
    // set (that it is set is what counts). Git for Windows' sh, which
    // runs hooks and aliases, never looks there.
    env.insert(QStringLiteral("NoDefaultCurrentDirectoryInExePath"), QStringLiteral("1"));
#endif
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
    // on kAskpassModeEnv — ssh passes no flags, so an env marker is
    // the only reliable switch). SSH_ASKPASS_REQUIRE=force makes
    // OpenSSH ≥ 8.4 use the askpass even when a TTY exists.
    //
    // Without prompts (forbidPrompts), git gets an empty GIT_ASKPASS:
    // it outranks core.askPass and SSH_ASKPASS, and with
    // GIT_TERMINAL_PROMPT=0 git fails at once, saying "terminal
    // prompts disabled". (A failing askpass would block it too, but
    // git's first line would then blame the askpass.) ssh still gets
    // GitBolt, in refuse mode, which says no without a window: an ssh
    // older than 8.4 ignores SSH_ASKPASS_REQUIRE=never, and could run
    // a distro ssh-askpass instead. Git Credential Manager opens a
    // sign-in window of its own unless told not to.
    const QString self = QCoreApplication::applicationFilePath();
    if (!allowPrompts) {
        env.insert(QStringLiteral("GIT_ASKPASS"), QString());
        env.insert(QStringLiteral("GCM_INTERACTIVE"), QStringLiteral("never"));
    }
    if (!self.isEmpty()) {
        if (allowPrompts)
            env.insert(QStringLiteral("GIT_ASKPASS"), self);
        env.insert(QStringLiteral("SSH_ASKPASS"), self);
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"),
                   QStringLiteral("force"));
        env.insert(QLatin1String(kAskpassModeEnv),
                   QLatin1String(allowPrompts ? kAskpassAsk : kAskpassRefuse));
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
    return runImpl(args, timeout.value_or(defaultTimeoutMs()));
}

Result<ProcessOutput> GitProcess::runWithInput(
    const std::vector<std::string>& args,
    const std::string& stdinData,
    std::optional<int> timeout) const
{
    return runImpl(args, timeout.value_or(defaultTimeoutMs()), &stdinData);
}

Result<void> GitProcess::checkOperand(const std::string& name) {
    if (name.empty() || name.front() != '-')
        return Result<void>::success();
    return GitError(GitErrorCode::InvalidSpec,
                    "\"" + name + "\" begins with '-', which git would read as an option, "
                    "not a name, so GitBolt doesn't pass it to git.");
}

Result<std::vector<std::string>> GitProcess::logOneline(const std::string& range, int maxCount) const {
    if (auto refused = checkOperand(range); !refused) return refused.error();
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
    for (const std::string* rev : {&from, &to})
        if (auto refused = checkOperand(*rev); !refused) return refused.error();
    auto result = run({"diff", from, to, "--"});
    if (!result) return result.error();
    return result->stdoutData;
}

Result<std::string> GitProcess::showFile(const std::string& revision, const std::string& path) const {
    if (auto refused = checkOperand(revision); !refused) return refused.error();
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
    //
    // The names come from the repository: a branch in the sidebar, the
    // remote it was pushed to. A branch named "--receive-pack=<command>"
    // (git allows the name) went to git as an option, and against a
    // remote on a local path git ran the command. So they are checked
    // first, the operands come after "--", and the branch goes as a
    // full ref, which can't begin with '-' either. A full ref also
    // pushes the branch where a tag shares its name, which the short
    // one refused as matching more than one ref.
    for (const std::string* name : {&remote, &branch})
        if (auto refused = checkOperand(*name); !refused) return refused.error();
    std::vector<std::string> args;
    args.reserve(6);
    args.emplace_back("push");
    if (force) args.emplace_back("--force-with-lease");
    if (setUpstream) args.emplace_back("--set-upstream");
    if (!remote.empty() || !branch.empty()) args.emplace_back("--");
    if (!remote.empty()) args.push_back(remote);
    if (!branch.empty())
        args.push_back(branch.rfind("refs/", 0) == 0 ? branch : "refs/heads/" + branch);
    return run(args, 120000);
}

Result<ProcessOutput> GitProcess::deleteRemoteBranch(const std::string& remote,
                                                     const std::string& branch) const {
    // As push() above: `git push <remote> --delete <branch>` read a
    // branch named "-x" (a remote's "origin/-x") as an option. The
    // short name stays: git deletes a full ref the server doesn't have
    // without a word, and a branch someone else deleted already is to
    // be reported as such.
    //
    // Only the remote is checked. After "--" the branch is a refspec
    // and nothing else, which --delete makes ":<branch>", so a branch
    // named like an option is deleted as any other; refusing it left
    // such a branch, on a server everyone fetches from, for the shell
    // to clear away.
    if (auto refused = checkOperand(remote); !refused) return refused.error();
    return run({"push", "--delete", "--", remote, branch}, 120000);
}

Result<ProcessOutput> GitProcess::pull(const std::string& remote, const std::string& branch) const {
    // Same empty-arg avoidance as push — `git pull origin ""` errors
    // out, but bare `git pull origin` (or just `git pull`) honors the
    // tracking branch. Same check of the names, too.
    for (const std::string* name : {&remote, &branch})
        if (auto refused = checkOperand(*name); !refused) return refused.error();
    std::vector<std::string> args;
    args.reserve(4);
    args.emplace_back("pull");
    if (!remote.empty() || !branch.empty()) args.emplace_back("--");
    if (!remote.empty()) args.push_back(remote);
    if (!branch.empty()) args.push_back(branch);
    return run(args, 120000);
}

Result<ProcessOutput> GitProcess::fetch(const std::string& remote, bool pruneByDefault) const {
    if (auto refused = checkOperand(remote); !refused) return refused.error();
    // `-c fetch.prune=true`, not --prune: a remote's own
    // remote.<name>.prune outranks fetch.prune even from -c, so a
    // remote set not to prune keeps its refs, where --prune would
    // override it. It also needs no remote name: an empty `remote` is
    // whichever one git picks.
    std::vector<std::string> args;
    args.reserve(4);
    if (pruneByDefault) {
        args.emplace_back("-c");
        args.emplace_back("fetch.prune=true");
    }
    args.emplace_back("fetch");
    if (!remote.empty()) args.push_back(remote);
    return run(args, 120000);
}

// ---------------------------------------------------------------------------
// Clone, and every other git command
// ---------------------------------------------------------------------------
namespace {

constexpr int kStartTimeoutMs = 5000;
// How long a cancel can go unnoticed: the output loop wakes at least
// this often to check the flag.
constexpr int kPollMs = 50;
// Per step of stopping the process tree; bounded so a wedged child
// can never hang the worker (and through the thread pool, app exit).
constexpr int kTerminateWaitMs = 2000;

// The process tree of one git command: a `git clone`, or any run().
// git does the transfer in children — git-remote-https, ssh,
// index-pack, upload-pack for a local URL, GitBolt itself in askpass
// mode — and local work too: hooks, clean / smudge filters. Killing
// only git on cancel orphans them: they run on, and on Windows keep
// files inside the destination open, so it can't be removed.
// ProcessTree puts git in a group that one call stops as a whole:
//
//   Unix:    setsid() in the child makes git the leader of a new
//            process group; terminate() signals the group.
//   Windows: git is created inside a job object (the attribute is
//            applied by CreateProcess itself, so not even a child
//            spawned in git's first microseconds escapes);
//            terminate() sends Ctrl-C to git's console where it can
//            (interruptTree), then ends the job, and KILL_ON_JOB_CLOSE
//            kills whatever is left if GitBolt itself dies mid-clone.
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

// The Ctrl-Cs ignoreCtrlC has taken (see interruptTree).
std::atomic<unsigned> ctrlCsTaken{0};

// GitBolt's answer to the Ctrl-C it sends git (see interruptTree):
// GenerateConsoleCtrlEvent reaches every process on the console, the
// sender attached to it included, and the default handler would end
// GitBolt. A handler function rather than SetConsoleCtrlHandler(nullptr,
// TRUE): that flag is inherited, and every git started after it would
// ignore Ctrl-C.
BOOL WINAPI ignoreCtrlC(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    ++ctrlCsTaken;
    return TRUE;
}

// Ctrl-C for git and everything it started, as in a terminal: they all
// share the hidden console that Qt's CREATE_NO_WINDOW gives git, and
// GitBolt attaches to it just long enough to send one. git's handler
// rolls back the lockfiles it holds (index.lock, HEAD.lock) and git
// exits; sh, a hook, a filter and ssh get it too. TerminateJobObject
// alone, like any TerminateProcess, runs none of that: git's
// index.lock stayed behind, and every later stage, commit and checkout
// failed on it. False when no Ctrl-C can be sent: GitBolt has a
// console of its own (a console test does; the app never has), which
// git then shares with whatever else runs there, or git is gone.
//
// The handler goes in after AttachConsole, every time: attaching puts
// the process's handler list back to the default one alone, which ends
// the process (ExitProcess(STATUS_CONTROL_C_EXIT)). Registered once,
// before the first attach, it was gone when the Ctrl-C came, and GitBolt
// ended with git: a quit, a repository switch during an auto-fetch, a
// clone's Cancel or any timeout took the whole app down (the Windows
// CI's test_git_process died "Interrupt" in its timeout test). Removed
// first, so that where attaching resets nothing it is listed once. The
// Ctrl-C reaches GitBolt on a thread of its own, after the call: the
// console stays attached, and the next cancel waits its turn, until the
// handler has taken it (or a second has gone by).
bool interruptTree(const QProcess& process)
{
    const auto pid = static_cast<DWORD>(process.processId());
    if (pid == 0)
        return false;
    // A process has one console at a time: two cancels take turns.
    static std::mutex consoleMutex;
    const std::lock_guard<std::mutex> lock(consoleMutex);
    if (!AttachConsole(pid))
        return false;
    SetConsoleCtrlHandler(ignoreCtrlC, FALSE);
    if (!SetConsoleCtrlHandler(ignoreCtrlC, TRUE)) {
        FreeConsole();
        return false;
    }
    const unsigned before = ctrlCsTaken.load();
    const bool sent = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != 0;
    if (sent) {
        QElapsedTimer waited;
        waited.start();
        while (ctrlCsTaken.load() == before && waited.elapsed() < 1000)
            QThread::msleep(5);
    }
    FreeConsole();
    return sent;
}

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
    // Ctrl-C first, so that git rolls back its lockfiles; then the job
    // ends whatever is left, or everything at once where no Ctrl-C
    // could be sent (a lockfile git held then stays behind). git exits
    // within milliseconds of the Ctrl-C, but a hook it ran gets one too
    // and may have cleaning up to do: pre-commit puts back the unstaged
    // changes it set aside. Ended with git, the hook lost them from the
    // work tree. So the job's other processes get the same time to
    // finish before it is ended.
    if (interruptTree(process)) {
        process.waitForFinished(kTerminateWaitMs);
        if (job_) {
            QElapsedTimer grace;
            grace.start();
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            while (grace.elapsed() < kTerminateWaitMs
                   && QueryInformationJobObject(job_, JobObjectBasicAccountingInformation,
                                                &info, static_cast<DWORD>(sizeof(info)),
                                                nullptr)
                   && info.ActiveProcesses > 0)
                QThread::msleep(10);
        }
    }
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
    // SIGINT first, what Ctrl-C in a terminal sends: git clone's
    // handler deletes the half-written destination itself, every git
    // rolls back the lockfiles it holds (index.lock, HEAD.lock), and ssh
    // closes its connection. A hook gets it too, and may have cleaning
    // up to do once git is gone: pre-commit puts back the unstaged
    // changes it set aside, in a trap or a Python `finally` (which
    // SIGTERM's default action skips). git exits within milliseconds,
    // and the SIGKILL that came right after it used to end that cleanup
    // halfway: quitting during a commit lost the user's unstaged changes
    // from the work tree. So the whole group gets the time git got
    // before SIGKILL sweeps it for anything that ignored SIGINT or hung.
    // kill(…, 0) fails once the group is empty (ESRCH), or holds only
    // zombies (EPERM, on macOS).
    ::kill(-group, SIGINT);
    process.waitForFinished(kTerminateWaitMs);
    QElapsedTimer grace;
    grace.start();
    while (::kill(-group, 0) == 0 && grace.elapsed() < kTerminateWaitMs)
        QThread::msleep(10);
    ::kill(-group, SIGKILL);
    process.waitForFinished(kTerminateWaitMs);
}

void ProcessTree::release()
{
    // Nothing to let go of: the group is only ever signalled on cancel.
}

#endif

// A git command on a thread of the global pool, where every long git job
// runs (a commit and its hooks, a rebase step, a checkout, a local op, a
// fetch), spends its time waiting on git, and the thread is one of the
// pool's idealThreadCount() slots all the while. The pool also opens
// repositories and runs every refresh: on a 2-CPU machine two such jobs
// held both slots, and an open waited the whole commit out ("Opening…"
// for as long as a pre-commit hook ran), the window's refreshes with it.
// While git runs, the slot goes back to the pool, which starts another
// thread if queued work needs one, and it is taken back once git is
// done. (QThreadPool::releaseThread, made for a thread that waits.)
class PoolSlotLent {
public:
    PoolSlotLent()
        : pool_(QThreadPool::globalInstance()->contains(QThread::currentThread())
                    ? QThreadPool::globalInstance() : nullptr) {
        if (pool_)
            pool_->releaseThread();
    }
    ~PoolSlotLent() {
        if (pool_)
            pool_->reserveThread();
    }
    PoolSlotLent(const PoolSlotLent&) = delete;
    PoolSlotLent& operator=(const PoolSlotLent&) = delete;

private:
    QThreadPool* const pool_;
};

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

// Every git command: git in a ProcessTree, and a poll loop instead of
// one long waitForFinished(), so a cancel is seen within kPollMs and
// stops the whole tree — including an ssh or git-remote-https waiting
// on a network that stopped answering, which would otherwise hold its
// caller until the timeout. A timeout stops the tree the same way. It
// used to SIGKILL git alone: git left its index.lock behind, which
// every later stage, commit and checkout tripped over, and the hook or
// filter it was running ran on, orphaned (a merge whose post-merge
// hook was slow had gone through, and was reported as timed out). Now
// git is interrupted first (SIGINT, as Ctrl-C sends, on Unix; a Ctrl-C
// on Windows; see ProcessTree::terminate) and rolls its lockfiles back
// itself, and a hook gets to finish cleaning up before the rest is
// killed.
Result<ProcessOutput> GitProcess::runImpl(const std::vector<std::string>& args, int timeoutMs,
                                          const std::string* stdinData,
                                          const std::map<std::string, std::string>& extraEnv) const
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
    applyEnvironment(process, !promptsForbidden_);
    if (!extraEnv.empty() || !gitDir_.empty()) {
        QProcessEnvironment env = process.processEnvironment();
        if (!gitDir_.empty())
            env.insert(QStringLiteral("GIT_DIR"), QString::fromStdString(gitDir_));
        for (const auto& [name, value] : extraEnv)
            env.insert(QString::fromStdString(name), QString::fromStdString(value));
        process.setProcessEnvironment(env);
    }
    tree.prepare(process);

    // The first git on the PATH its children get (applyEnvironment): the
    // git a hook, an alias or the Console runs is this one too.
    const QString gitPath =
        gitOnPath(process.processEnvironment().value(QStringLiteral("PATH")));

    // Wall-clock timing the run so the command log can show per-
    // command latency.
    QElapsedTimer timer;
    timer.start();
    if (gitPath.isEmpty()) {
        GitProcessLog::instance().emitCommand(workDir, qargs, -1, timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed, kNoGit);
    }
    process.start(gitPath, qargs);
    bool started = process.waitForStarted(kStartTimeoutMs);
    if (!started && tree.dropHook(process)) {
        process.start(gitPath, qargs);
        started = process.waitForStarted(kStartTimeoutMs);
    }
    if (!started) {
        // Still logged, with exit code -1: someone debugging "why
        // didn't this run?" wants to see the attempts too.
        GitProcessLog::instance().emitCommand(workDir, qargs, -1, timer.elapsed());
        return GitError(GitErrorCode::ProcessFailed, "Failed to start git process");
    }
    // Fed once and closed, so that git reading it (`commit -F -`,
    // `apply -`) sees the end: they block until the channel closes.
    if (stdinData) {
        process.write(stdinData->data(), static_cast<qint64>(stdinData->size()));
        process.closeWriteChannel();
    }

    // Each wait also moves git's output into QProcess's buffers (and
    // the payload into its stdin), so its pipes can't fill up and
    // stall it between polls.
    const PoolSlotLent lent;
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
    // As for run(): the first git on the PATH its children get.
    const QString gitPath = gitOnPath(env.value(QStringLiteral("PATH")));
    if (gitPath.isEmpty()) {
        GitProcessLog::instance().emitCommand(logDir, logArgs, -1, 0);
        return GitError(GitErrorCode::ProcessFailed, kNoGit);
    }
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
    const PoolSlotLent lent;
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
    // own state, for kEditor and rebaseConfig() to find: the
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

    // Through runImpl() like every other command, so a cancel (quitting)
    // stops it and its hooks; it used to run a QProcess of its own that
    // nothing could stop, and quitting waited out the whole rebase. The
    // git commands the rebase runs (`git commit` for a reword or a
    // squash) inherit the -c.
    // The new messages are UTF-8, and so are the commits' (kUtf8Commits).
    auto result = runImpl({"-c", kUtf8Commits, "-c", std::string("core.commentChar=") + commentChar,
                           "rebase", "-i", onto},
                          kRebaseTimeoutMs, nullptr,
                          {{"GIT_SEQUENCE_EDITOR", sequenceEditor.toStdString()}});
    keepRewordMessage();
    return result;
}

std::vector<std::string> GitProcess::rebaseConfig() const {
    std::vector<std::string> config = {"-c", kUtf8Commits};
    const auto path = run({"rev-parse", "--git-path",
                           QStringLiteral("rebase-merge/%1/%2")
                               .arg(kRewordMessages, kCommentChar).toStdString()});
    if (!path || !path->success())
        return config;
    QFile file(QDir(QString::fromStdString(workDir_))
                   .absoluteFilePath(QString::fromStdString(path->stdoutData).trimmed()));
    if (!file.open(QIODevice::ReadOnly))
        return config;
    const QByteArray commentChar = file.read(2);
    if (commentChar.size() != 1)
        return config;
    config.insert(config.end(), {"-c", "core.commentChar=" + commentChar.toStdString()});
    return config;
}

QString GitProcess::gitPath(const QString& path) const {
    const auto out = run({"rev-parse", "--git-path", path.toStdString()});
    if (!out || !out->success())
        return {};
    return QDir(QString::fromStdString(workDir_))
        .absoluteFilePath(QString::fromStdString(out->stdoutData).trimmed());
}

// git's done list ends with the line of the commit the rebase stopped
// at ("reword <hash> <subject>", as kEditor reads it too).
QString GitProcess::stoppedRewordMessage(std::string* hash) const {
    const QString dir = gitPath(QStringLiteral("rebase-merge"));
    if (dir.isEmpty())
        return {};
    QFile done(dir + QStringLiteral("/done"));
    if (!done.open(QIODevice::ReadOnly))
        return {};
    const QList<QByteArray> words = done.readAll().trimmed().split('\n').last().simplified().split(' ');
    if (words.size() < 2 || (words[0] != "reword" && words[0] != "r"))
        return {};
    const QString message = QStringLiteral("%1/%2/%3")
                                .arg(dir, kRewordMessages, QString::fromLatin1(words[1]));
    if (!QFileInfo(message).isFile())
        return {};
    if (hash)
        *hash = words[1].toStdString();
    return message;
}

// HEAD is the reword's commit as git picked it, before the message:
// the commit itself (git fast-forwards where it can), or a copy that
// keeps its author, date and message, and its change (patch-id). The
// message alone said so of whichever commit HEAD was: a stop before the
// reword's own was made (an untracked file in the way, a conflict
// resolved to nothing) left an earlier commit with the same message
// there ("wip", "wip"), and Continue renamed that one.
std::optional<bool> GitProcess::headIsPickOf(const std::string& hash) const {
    // `hash` is read from git's rebase state in the repository, which
    // holds whatever was written there: a word that begins with '-'
    // went to log and diff-tree below as an option.
    if (!checkOperand(hash))
        return std::nullopt;
    const auto ids = run({"rev-parse", "HEAD", hash + "^{commit}"});
    if (!ids || !ids->success())
        return std::nullopt;
    const QStringList both = QString::fromStdString(ids->stdoutData)
                                 .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (both.size() == 2 && both[0] == both[1])
        return true;
    // Who and when, not the message: a prepare-commit-msg hook that
    // adds to every message it's given ("[T-1]", a Change-Id) gave the
    // pick another one, and Continue's retry of a reword a commit-msg
    // hook had turned down never came ("Rebase complete.", the reword
    // gone). The patch-id below pins the change, which tells "wip" from
    // "wip".
    const auto commit = [&](const std::string& rev, const char* format) {
        return run({"log", "-1", "--date=raw", std::string("--format=") + format, rev});
    };
    const auto now = commit("HEAD", "%an%n%ae%n%ad");
    const auto before = commit(hash, "%an%n%ae%n%ad");
    if (!now || !before || !now->success() || !before->success())
        return std::nullopt;
    if (now->stdoutData != before->stdoutData)
        return false;
    // The change: the same patch-id, whatever base it was picked onto.
    // Without context lines (-U0): a clean pick onto a base that
    // changed lines near the commit's has other context around the
    // same change, and with it another patch-id. The pick then didn't
    // count as the commit, and Skip at an `edit` stop on a rebase onto
    // an updated branch ran git's reset --hard over the changes made
    // there.
    const auto patchId = [&](const std::string& rev) -> std::optional<std::string> {
        const auto patch = run({"diff-tree", "--root", "-p", "-U0", "--no-commit-id", rev});
        if (!patch || !patch->success())
            return std::nullopt;
        if (patch->stdoutData.empty())
            return std::string();   // no change at all
        const auto id = runWithInput({"patch-id", "--stable"}, patch->stdoutData);
        if (!id || !id->success() || id->stdoutData.empty())
            return std::nullopt;
        return id->stdoutData.substr(0, id->stdoutData.find(' '));
    };
    const auto headId = patchId("HEAD");
    const auto hashId = patchId(hash);
    if (!headId || !hashId)
        return std::nullopt;
    if (*headId != *hashId)
        return false;
    if (!headId->empty())
        return true;
    // No change to tell them apart by: the message has to start as the
    // commit's did, whatever a hook added after it.
    const auto nowMessage = commit("HEAD", "%B");
    const auto beforeMessage = commit(hash, "%B");
    if (!nowMessage || !beforeMessage || !nowMessage->success() || !beforeMessage->success())
        return std::nullopt;
    const QString was = QString::fromStdString(beforeMessage->stdoutData).trimmed();
    return QString::fromStdString(nowMessage->stdoutData).trimmed().startsWith(was);
}

std::string GitProcess::committedRebaseStop() const {
    const QString dir = gitPath(QStringLiteral("rebase-merge"));
    if (dir.isEmpty())
        return {};
    QFile done(dir + QStringLiteral("/done"));
    if (!done.open(QIODevice::ReadOnly))
        return {};
    const QList<QByteArray> words = done.readAll().trimmed().split('\n').last().simplified().split(' ');
    if (words.size() < 2)
        return {};
    std::string verb;
    if (words[0] == "pick" || words[0] == "p")
        verb = "pick";
    else if (words[0] == "reword" || words[0] == "r")
        verb = "reword";
    else if (words[0] == "edit" || words[0] == "e")
        verb = "edit";
    else
        return {};
    // git's own word for it: it writes rebase-merge/amend where it stops
    // with the commit made, to be amended (every `edit`, and a reword
    // turned down after a fast-forward), and removes it when the rebase
    // goes on. It stays through whatever the user does there: an amend
    // of the commit, or a split of it, leaves HEAD a commit that is no
    // pick of the stopped one, and Skip then ran git's reset --hard over
    // the work made since.
    if (QFileInfo::exists(dir + QStringLiteral("/amend")))
        return verb;
    // A pick git has yet to commit (a conflict, or a change that came
    // out empty): HEAD is the commit before it, which may be the same
    // change picked earlier (the branch's copy of it).
    if (QFileInfo::exists(gitPath(QStringLiteral("CHERRY_PICK_HEAD"))))
        return {};
    // Where git can't tell (a patch-id that timed out on a huge commit),
    // as made: rebaseSkip() then refuses while there is work to lose,
    // rather than letting a reset --hard through.
    const std::optional<bool> picked = headIsPickOf(words[1].toStdString());
    return !picked || *picked ? verb : std::string();
}

// The message git turned down is to be tried again where HEAD is still
// the commit the stop left: the stopped commit itself (a fast-forward),
// or git's own pick of it. Anything since (the user's amend of the
// message at the stop, "commit (amend): ...") is the user's: the plan's
// message, still turned down by the hook, came back on every Continue,
// and replaced theirs where the hook took it. What tells them apart:
// git's own record of the stop where it keeps one (rebase-merge/amend),
// else HEAD's reflog, else, with no reflog either, the message.
bool GitProcess::headIsTheRewordStop(const std::string& hash) const {
    if (!checkOperand(hash))    // as in headIsPickOf()
        return false;
    const auto ids = run({"rev-parse", "HEAD", hash + "^{commit}"});
    if (!ids || !ids->success())
        return false;
    const QStringList both = QString::fromStdString(ids->stdoutData)
                                 .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (both.size() == 2 && both[0] == both[1])
        return true;
    if (both.isEmpty())
        return false;
    // git writes HEAD to rebase-merge/amend where it stops with the
    // commit made, to be amended: at a reword turned down after a
    // fast-forward (HEAD then is the commit itself, above). A HEAD
    // other than that is a commit made since.
    if (QFile amend(gitPath(QStringLiteral("rebase-merge/amend")));
        amend.open(QIODevice::ReadOnly))
        return QString::fromLatin1(amend.readAll()).trimmed() == both[0];
    const auto entry = run({"log", "-g", "-1", "--format=%gs", "HEAD"});
    const QString subject = entry && entry->success()
        ? QString::fromStdString(entry->stdoutData).trimmed() : QString();
    if (subject.isEmpty()) {
        // No HEAD reflog to ask (core.logAllRefUpdates=false, and none
        // written before): git says nothing (2.39 exits 0). That ruled
        // the retry out, and Continue said "Rebase complete." with the
        // reword gone. git's pick keeps the old message (the reword is
        // what the hook turned down); the user's amend at the stop is
        // what changes it. The same message, then; or one that starts
        // with it where a prepare-commit-msg hook may have added to
        // git's pick, as in headIsPickOf(). Not otherwise: an amend that
        // kept the old message and added to it (a body, a trailer)
        // passed for the pick, and the retry put the plan's message,
        // turned down again, back over it on every Continue.
        const auto commit = [&](const std::string& rev) {
            return run({"log", "-1", "--format=%B", rev});
        };
        const auto now = commit("HEAD");
        const auto was = commit(hash);
        if (!now || !was || !now->success() || !was->success())
            return false;
        const QString nowMessage = QString::fromStdString(now->stdoutData).trimmed();
        const QString wasMessage = QString::fromStdString(was->stdoutData).trimmed();
        if (nowMessage == wasMessage)
            return true;
        return nowMessage.startsWith(wasMessage) && hasPrepareCommitMsgHook();
    }
    return subject.startsWith(QLatin1String("rebase (reword)"))
        || subject.startsWith(QLatin1String("rebase -i (reword)"));
}

// Where git looks for it (core.hooksPath honoured), as git runs it: Git
// for Windows runs any file there, through sh, and, where there's none,
// the name with ".exe" (find_hook()'s STRIP_EXTENSION retry), as a
// compiled hook has.
bool GitProcess::hasPrepareCommitMsgHook() const {
    const QString path = gitPath(QStringLiteral("hooks/prepare-commit-msg"));
    if (path.isEmpty())
        return false;
    const QFileInfo hook(path);
#ifdef Q_OS_WIN
    return hook.isFile() || QFileInfo(path + QLatin1String(".exe")).isFile();
#else
    return hook.isFile() && hook.isExecutable();
#endif
}

QString GitProcess::rejectedRewordMessage() const {
    std::string hash;
    const QString message = stoppedRewordMessage(&hash);
    if (message.isEmpty())
        return {};
    // A pick git has yet to commit (a conflict, or one that came out
    // empty on top of the branch's own copy of the change): that copy at
    // HEAD looked like the pick, and the retry's commit --amend failed
    // there ("cannot amend") on every Continue. git's --continue
    // commits, or drops, the pick.
    if (QFileInfo::exists(gitPath(QStringLiteral("CHERRY_PICK_HEAD"))))
        return {};
    const auto staged = run({"diff", "--cached", "--quiet"});
    if (!staged || staged->exitCode != 0)
        return {};
    const std::optional<bool> picked = headIsPickOf(hash);
    if (!picked || !*picked || !headIsTheRewordStop(hash))
        return {};
    return message;
}

// Only kEditor knew the new message: a `git rebase --continue` in a
// terminal, after the conflict that stopped a reword, committed with
// what these held, the old one.
void GitProcess::keepRewordMessage() const {
    const QString message = stoppedRewordMessage();
    if (message.isEmpty())
        return;
    for (const QString& kept : {gitPath(QStringLiteral("rebase-merge/message")),
                                gitPath(QStringLiteral("MERGE_MSG"))}) {
        if (!kept.isEmpty() && QFileInfo::exists(kept) && QFile::remove(kept))
            QFile::copy(message, kept);
    }
}

Result<ProcessOutput> GitProcess::rebaseContinue() const {
    const std::vector<std::string> commentConfig = rebaseConfig();
    // A reword whose new message a hook turned down (commit-msg, say)
    // stopped the rebase with the commit picked under its old message.
    // git's --continue then went on with that message, and Continue
    // said "Rebase complete." with the reword gone. It gets the plan's
    // message again first, hooks and all.
    if (const QString message = rejectedRewordMessage(); !message.isEmpty()) {
        std::vector<std::string> amend = commentConfig;
        amend.insert(amend.end(), {"commit", "--amend", "--only", "--allow-empty", "-F",
                                   QDir::toNativeSeparators(message).toStdString()});
        auto amended = run(amend, kRebaseTimeoutMs);
        if (!amended || !amended->success())
            return amended;
    }
    std::vector<std::string> args = commentConfig;
    args.insert(args.end(), {"rebase", "--continue"});
    auto result = run(args, kRebaseTimeoutMs);
    keepRewordMessage();
    return result;
}

Result<ProcessOutput> GitProcess::rebaseAbort() const {
    return run({"rebase", "--abort"}, kRebaseTimeoutMs);
}

bool GitProcess::hasTrackedChanges() const {
    // --no-optional-locks: a look, which mustn't take index.lock from a
    // git that runs meanwhile. Submodules: a dirty one's work tree is
    // its own, which no reset of this one touches.
    const auto status = run({"--no-optional-locks", "status", "--porcelain",
                             "--untracked-files=no", "--ignore-submodules=dirty"});
    if (!status || !status->success())
        return true;
    return !QString::fromStdString(status->stdoutData).trimmed().isEmpty();
}

Result<ProcessOutput> GitProcess::rebaseSkip() const {
    // At an `edit` (or a reword a hook turned down) HEAD is the stopped
    // commit, and the paused bar says to make changes there; Continue
    // turns unstaged ones down, which left Skip, and git's --skip runs
    // `reset --hard` first: the edits to tracked files, and new files
    // staged, were gone for good, with "Rebase complete.".
    if (!committedRebaseStop().empty() && hasTrackedChanges())
        return GitError(GitErrorCode::Uncommitted, kSkipWouldDiscard);
    std::vector<std::string> args = rebaseConfig();
    args.insert(args.end(), {"rebase", "--skip"});
    auto result = run(args, kRebaseTimeoutMs);
    keepRewordMessage();
    return result;
}

Result<ProcessOutput> GitProcess::sequenceContinue(bool revert) const {
    return run({revert ? "revert" : "cherry-pick", "--continue"}, kRebaseTimeoutMs);
}

bool GitProcess::stagedWorkWithoutPick() const {
    for (const char* pending : {"CHERRY_PICK_HEAD", "REVERT_HEAD"}) {
        const QString path = gitPath(QString::fromLatin1(pending));
        if (path.isEmpty())
            return true;    // git can't say: as if there were
        if (QFileInfo::exists(path))
            return false;
    }
    const auto staged = run({"--no-optional-locks", "diff", "--cached", "--quiet",
                             "--ignore-submodules=dirty"});
    return !staged || staged->exitCode != 0;
}

std::string GitProcess::unpickedSequenceStop() const {
    // A pick or revert pending: git's --continue commits it.
    for (const char* pending : {"CHERRY_PICK_HEAD", "REVERT_HEAD"}) {
        const QString path = gitPath(QString::fromLatin1(pending));
        if (path.isEmpty() || QFileInfo::exists(path))
            return {};
    }
    // The todo's first line is the commit git stopped at: its --continue
    // takes it for done (todo_list.current++), picked or not.
    QFile todo(gitPath(QStringLiteral("sequencer/todo")));
    if (!todo.open(QIODevice::ReadOnly))
        return {};
    QByteArray line;
    while (!todo.atEnd() && line.isEmpty()) {
        line = todo.readLine().simplified();
        if (line.startsWith('#'))
            line.clear();
    }
    const qsizetype verbEnd = line.indexOf(' ');
    if (verbEnd < 0)
        return {};
    const QByteArray verb = line.left(verbEnd);
    const QByteArray rest = line.mid(verbEnd + 1);
    const std::string stop = rest.left(rest.indexOf(' ') < 0 ? rest.size() : rest.indexOf(' '))
                                 .toStdString();
    bool revert = false;
    if (verb == "revert")
        revert = true;
    else if (verb != "pick" && verb != "p")
        return {};
    // From the repository's sequencer state, as headIsPickOf()'s hash is.
    if (!checkOperand(stop))
        return {};
    const auto full = run({"rev-parse", "--verify", "--quiet", stop + "^{commit}"});
    if (!full || !full->success())
        return {};
    const QString stopId = QString::fromStdString(full->stdoutData).trimmed();

    // The commits made since the sequence's own last step: git writes
    // HEAD to abort-safety after each commit of its own. Where it
    // hasn't one (an older git), HEAD alone.
    QByteArray since;
    if (QFile safety(gitPath(QStringLiteral("sequencer/abort-safety")));
        safety.open(QIODevice::ReadOnly))
        since = safety.readAll().trimmed();
    // Each "<id>\n" and then what tells it.
    std::vector<std::string> range = {"log", "-z", "--date=raw",
                                      since.isEmpty() ? "-n1" : "-n100",
                                      revert ? "--format=%H%n%B" : "--format=%H%n%an%n%ae%n%ad",
                                      "HEAD"};
    if (!since.isEmpty())
        range.push_back("^" + since.toStdString());
    const auto made = run(range);
    if (!made || !made->success())
        return {};
    QStringList ids;
    QStringList commits;
    for (const QString& commit : QString::fromStdString(made->stdoutData)
                                     .split(QLatin1Char('\0'), Qt::SkipEmptyParts)) {
        const qsizetype end = commit.indexOf(QLatin1Char('\n'));
        ids << commit.left(end).trimmed();
        commits << (end < 0 ? QString() : commit.mid(end + 1));
    }
    // A commit of GitBolt's that concluded the stop (commit() notes
    // it), and is still there: whatever its message. The Commit
    // dialog's commit carries the words typed, and a revert's "This
    // reverts commit" wasn't among them.
    if (QFile concluded(gitPath(kConcludedStops)); concluded.open(QIODevice::ReadOnly)) {
        for (const QByteArray& noted : concluded.readAll().split('\n')) {
            const QList<QByteArray> pair = noted.simplified().split(' ');
            if (pair.size() == 2 && QString::fromLatin1(pair[0]) == stopId
                && ids.contains(QString::fromLatin1(pair[1])))
                return {};
        }
    }
    if (revert) {
        // git's message, kept through a conflict's MERGE_MSG: "This
        // reverts commit <id>." (abbreviated with revert.reference).
        static const QRegularExpression reverts(
            QStringLiteral("This reverts commit ([0-9a-f]{7,})"));
        for (const QString& message : commits) {
            for (auto it = reverts.globalMatch(message); it.hasNext();) {
                if (stopId.startsWith(it.next().captured(1)))
                    return {};
            }
        }
    } else {
        const auto author = run({"log", "-1", "--date=raw", "--format=%an%n%ae%n%ad", stop});
        if (!author || !author->success())
            return {};
        const QString picked = QString::fromStdString(author->stdoutData).trimmed();
        for (const QString& who : commits) {
            if (who.trimmed() == picked)
                return {};
        }
    }
    return rest.toStdString();
}

Result<ProcessOutput> GitProcess::sequenceSkip(bool revert) const {
    // A sequence git stopped before it picked anything for the commit
    // (a pick it refused over a dirty index): there's no pick to drop,
    // and git's --skip runs `reset --merge HEAD`, which put every staged
    // change back to HEAD's, with "Cherry-pick complete.".
    if (stagedWorkWithoutPick())
        return GitError(GitErrorCode::Uncommitted, kSequenceSkipWouldDiscard);
    return run({revert ? "revert" : "cherry-pick", "--skip"}, kRebaseTimeoutMs);
}

Result<ProcessOutput> GitProcess::commit(const std::string& message, bool amend) const {
    // The pick or revert, in a sequence of several, that this commit
    // concludes (a conflict resolved): noted for unpickedSequenceStop(),
    // as the message typed here need not be git's.
    QString concluding;
    if (const QString sequencer = gitPath(QStringLiteral("sequencer"));
        !sequencer.isEmpty() && QFileInfo(sequencer).isDir()) {
        for (const char* pending : {"CHERRY_PICK_HEAD", "REVERT_HEAD"}) {
            QFile head(gitPath(QString::fromLatin1(pending)));
            if (head.open(QIODevice::ReadOnly)) {
                concluding = QString::fromLatin1(head.readAll()).trimmed();
                break;
            }
        }
        // An amend of a commit noted so (a typo fixed in it) replaces
        // it, and concludes the stop in its place: the note named the
        // commit that went, and a revert's in the user's words was
        // taken for one git never made, so Continue advised Abort.
        if (concluding.isEmpty() && amend) {
            const auto head = run({"rev-parse", "--verify", "--quiet", "HEAD"});
            QFile concluded(gitPath(kConcludedStops));
            if (head && head->success() && concluded.open(QIODevice::ReadOnly)) {
                const QByteArray was = QByteArray::fromStdString(head->stdoutData).trimmed();
                for (const QByteArray& noted : concluded.readAll().split('\n')) {
                    const QList<QByteArray> pair = noted.simplified().split(' ');
                    if (pair.size() == 2 && pair[1] == was)
                        concluding = QString::fromLatin1(pair[0]);
                }
            }
        }
    }
    // A reword the rebase stopped at: the user's commit there (an amend
    // of its message, say) is theirs, and the plan's message, which a
    // hook turned down, is not to come back over it on Continue
    // (rejectedRewordMessage()).
    const QString rewordPlan = stoppedRewordMessage();

    // The message is UTF-8, and labelled so (kUtf8Commits).
    std::vector<std::string> args = {"-c", kUtf8Commits, "commit", "-F", "-"};
    if (amend)
        args.emplace_back("--amend");
    auto result = runImpl(args, kCommitTimeoutMs, &message);
    if (!result || !result->success())
        return result;
    if (!concluding.isEmpty()) {
        const auto head = run({"rev-parse", "HEAD"});
        QFile concluded(gitPath(kConcludedStops));
        if (head && head->success() && concluded.open(QIODevice::WriteOnly | QIODevice::Append)) {
            concluded.write(concluding.toLatin1() + ' '
                            + QByteArray::fromStdString(head->stdoutData).trimmed() + '\n');
        }
    }
    if (!rewordPlan.isEmpty())
        QFile::remove(rewordPlan);
    return result;
}

// git-flow parses its arguments with shFlags, which takes a name that
// begins with '-' (a feature typed so, or one of the repository's
// feature/ branches named so) for a flag of its own.
namespace {
Result<ProcessOutput> gitFlow(const GitProcess& git, const char* kind, const char* verb,
                              const std::string& name) {
    if (auto refused = GitProcess::checkOperand(name); !refused) return refused.error();
    return git.run({"flow", kind, verb, name});
}
} // namespace

Result<ProcessOutput> GitProcess::gitFlowInit() const { return run({"flow", "init", "-d"}); }
Result<ProcessOutput> GitProcess::gitFlowFeatureStart(const std::string& n) const { return gitFlow(*this, "feature", "start", n); }
Result<ProcessOutput> GitProcess::gitFlowFeatureFinish(const std::string& n) const { return gitFlow(*this, "feature", "finish", n); }
Result<ProcessOutput> GitProcess::gitFlowReleaseStart(const std::string& v) const { return gitFlow(*this, "release", "start", v); }
Result<ProcessOutput> GitProcess::gitFlowReleaseFinish(const std::string& v) const { return gitFlow(*this, "release", "finish", v); }
Result<ProcessOutput> GitProcess::gitFlowHotfixStart(const std::string& v) const { return gitFlow(*this, "hotfix", "start", v); }
Result<ProcessOutput> GitProcess::gitFlowHotfixFinish(const std::string& v) const { return gitFlow(*this, "hotfix", "finish", v); }

Result<ProcessOutput> GitProcess::gc() const { return run({"gc", "--auto"}, 300000); }
Result<ProcessOutput> GitProcess::prune() const { return run({"prune"}, 120000); }
Result<ProcessOutput> GitProcess::fsck() const { return run({"fsck", "--full"}, 300000); }

std::string GitProcess::findGitExecutable() {
    return gitOnPath(util::pathForChildren(qEnvironmentVariable("PATH"))).toStdString();
}

std::string GitProcess::findGitOnPath(const std::string& pathList,
                                      const std::vector<std::string>& fallbacks) {
    const QString found = util::findExecutableOnPath(QStringLiteral("git"),
                                                     QString::fromStdString(pathList));
    if (!found.isEmpty())
        return found.toStdString();
    for (const std::string& fallback : fallbacks)
        if (QFileInfo(QString::fromStdString(fallback)).isExecutable())
            return fallback;
    return {};
}

} // namespace gitbolt::git
