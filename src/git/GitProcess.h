#pragma once

#include "git/CloneProgress.h"
#include "git/Error.h"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QProcess;
class QString;

namespace gitbolt::git {

struct ProcessOutput {
    int exitCode;
    std::string stdoutData;
    std::string stderrData;

    bool success() const { return exitCode == 0; }
};

class GitProcess {
public:
    explicit GitProcess(const std::string& workingDirectory);

    /// Without `timeoutMs`, git gets defaultTimeoutMs(). git runs in a
    /// process group of its own (a job object on Windows), as clone()
    /// does: a timeout, or a cancel (addCancelFlag), stops git and
    /// every process it started, SIGTERM first so git can roll back
    /// its lockfiles, as on Ctrl-C.
    Result<ProcessOutput> run(const std::vector<std::string>& args,
                              std::optional<int> timeoutMs = std::nullopt) const;

    /// How long run() lets git take when the caller names no limit:
    /// 30 s, or GITBOLT_GIT_TIMEOUT_MS when that is set. Tests shorten
    /// it to show, in seconds rather than minutes, that a command
    /// which may run longer names a limit of its own.
    static int defaultTimeoutMs();

    /// Makes run(), and every command built on it, stoppable. Once
    /// `flag` is set, from any thread, run() stops git and every
    /// process git started within ~50 ms and fails with
    /// GitErrorCode::User; later runs fail without starting git.
    /// Flags add up, and any one of them stops it: every git command
    /// GitService runs has the flag that quitting sets, and an
    /// auto-fetch one of its own as well, so it can be dropped alone.
    /// A null flag is ignored, so an optional one can be passed on.
    void addCancelFlag(std::shared_ptr<std::atomic<bool>> flag) {
        if (flag)
            cancelFlags_.push_back(std::move(flag));
    }

    /// Like run(), but feeds `stdinData` to the child's stdin and
    /// closes the write channel. Needed for commands that read a
    /// payload from stdin, e.g. `git apply --cached -` with a patch
    /// (hunk-level staging builds patches in memory; writing temp
    /// files just to hand git a few hundred bytes would be noise).
    Result<ProcessOutput> runWithInput(const std::vector<std::string>& args,
                                       const std::string& stdinData,
                                       std::optional<int> timeoutMs = std::nullopt) const;

    Result<std::vector<std::string>> logOneline(const std::string& range = "", int maxCount = -1) const;
    Result<std::string> diffRaw(const std::string& from, const std::string& to) const;
    Result<std::string> showFile(const std::string& revision, const std::string& path) const;

    /// setUpstream adds --set-upstream: publish a branch and record
    /// <remote>/<branch> as its upstream (what `git push -u` does).
    Result<ProcessOutput> push(const std::string& remote, const std::string& branch,
                               bool force = false, bool setUpstream = false) const;
    Result<ProcessOutput> pull(const std::string& remote, const std::string& branch) const;
    Result<ProcessOutput> fetch(const std::string& remote = "", bool prune = false) const;

    /// `git clone --progress -- <url> <path>`: the same CLI path push /
    /// pull / fetch take, so credential helpers, ssh config / agent /
    /// known_hosts, core.sshCommand and the askpass prompt all behave
    /// exactly as they do for git in a terminal. Blocks until git
    /// exits — call it from a worker thread.
    ///
    /// `onProgress` runs on the calling thread with each update parsed
    /// from git's stderr (CloneProgressParser). Setting `cancelFlag`
    /// from any thread stops git and every process it started within
    /// a poll interval (~50 ms).
    ///
    /// Refuses a destination that exists and is not an empty directory
    /// (as git does). After a failure or cancel the destination is put
    /// back the way it was — removed, or emptied if it already existed
    /// — so a retry to the same path works. The error message is the
    /// tail of git's stderr ("fatal: …").
    ///
    /// The exception: once the clone itself is complete (HEAD names
    /// the fetched commit), git keeps the repository whatever fails
    /// next — the checkout, or a post-checkout hook after it — and so
    /// does this. The error is then CheckoutFailed; its message says
    /// where the repository is and which of the two failed, and ends
    /// with git's advice on finishing the checkout or the hook's
    /// output.
    static Result<void> clone(const std::string& url, const std::string& path,
                              const CloneProgressCallback& onProgress = nullptr,
                              const std::shared_ptr<std::atomic<bool>>& cancelFlag = nullptr);

    /// `url` with its credentials masked, for display (the command
    /// log): "https://user:secret@host/…" → "https://user:***@host/…",
    /// and a token pasted as the user name, "https://<token>@host/…",
    /// → "https://***@host/…". An ssh user name ("ssh://git@host/…",
    /// "git@host:path") is no secret and is kept.
    static std::string redactUrl(const std::string& url);

    /// `git rebase -i <onto>` with `todo` as its todo list, in git's
    /// format and order ("pick <hash> <subject>" lines, oldest first).
    /// `newMessages` maps the full hash of each commit the todo
    /// rewords to its new message; the commit gets it when git asks
    /// for it, from this call or from a later rebaseContinue() /
    /// rebaseSkip(). A reword without an entry keeps its message.
    /// `messages` are the messages the todo's commits have now, which
    /// git can put in a commit too (a squash keeps them): git runs the
    /// rebase with a core.commentChar that starts no line of them or
    /// of a new one, so it strips nothing they say. rebaseContinue()
    /// and rebaseSkip() carry on with the same one.
    Result<ProcessOutput> interactiveRebase(const std::string& onto, const std::string& todo,
                                            const std::map<std::string, std::string>& newMessages = {},
                                            const std::vector<std::string>& messages = {}) const;
    /// Before git carries on, a reword whose new message a hook turned
    /// down (git kept the commit, under its old message) is committed
    /// with the plan's message again; while the hooks still say no, the
    /// rebase stays where it is and this fails with what they said.
    Result<ProcessOutput> rebaseContinue() const;
    Result<ProcessOutput> rebaseAbort() const;
    Result<ProcessOutput> rebaseSkip() const;

    /// `git cherry-pick --continue`, or `git revert --continue` when
    /// `revert`: carries on with a sequence of several commits (see
    /// Repository::sequencePaused()). Skip leaves out the commit the
    /// sequence stopped at first. As long as a rebase step may take.
    Result<ProcessOutput> sequenceContinue(bool revert) const;
    Result<ProcessOutput> sequenceSkip(bool revert) const;

    /// `git commit -F -` (`--amend` too when `amend`), the message on
    /// stdin: the commit a terminal's git would make. The repository's
    /// hooks run (core.hooksPath honoured), commit.gpgSign and
    /// commit.cleanup apply, and git concludes a merge, cherry-pick or
    /// revert in progress, a merge's autostash included. Up to five
    /// minutes, for slow hooks. git runs in a process group of its own,
    /// so a hook that hangs dies with it when that time is up or a
    /// cancel flag is set.
    Result<ProcessOutput> commit(const std::string& message, bool amend = false) const;

    Result<ProcessOutput> gitFlowInit() const;
    Result<ProcessOutput> gitFlowFeatureStart(const std::string& name) const;
    Result<ProcessOutput> gitFlowFeatureFinish(const std::string& name) const;
    Result<ProcessOutput> gitFlowReleaseStart(const std::string& version) const;
    Result<ProcessOutput> gitFlowReleaseFinish(const std::string& version) const;
    Result<ProcessOutput> gitFlowHotfixStart(const std::string& version) const;
    Result<ProcessOutput> gitFlowHotfixFinish(const std::string& version) const;

    Result<ProcessOutput> gc() const;
    Result<ProcessOutput> prune() const;
    Result<ProcessOutput> fsck() const;

    static std::string findGitExecutable();

private:
    /// Shared child-process environment: inherits the session env
    /// and sets GIT_TERMINAL_PROMPT=0 so a promptless child fails
    /// fast instead of hanging until the timeout kills it, and a
    /// GIT_EDITOR that never waits on anyone (see kEditor).
    static void applyEnvironment(QProcess& process);

    /// What run(), runWithInput() and interactiveRebase() share: git
    /// in a process tree, fed `stdinData` when given, with `extraEnv`
    /// on top of applyEnvironment()'s.
    Result<ProcessOutput> runImpl(const std::vector<std::string>& args, int timeoutMs,
                                  const std::string* stdinData = nullptr,
                                  const std::map<std::string, std::string>& extraEnv = {}) const;
    /// Whether any of cancelFlags_ is set.
    bool cancelled() const;

    /// "-c core.commentChar=<c>" with the comment character that
    /// interactiveRebase() started the rebase in progress with, for
    /// the git that carries it on: a reword that stopped on a conflict
    /// is committed then. Empty for a rebase started some other way,
    /// which keeps the user's own setting.
    std::vector<std::string> rebaseCommentConfig() const;

    /// `git rev-parse --git-path <path>`, made absolute; empty when git
    /// can't say (no repository).
    QString gitPath(const QString& path) const;

    /// The file with the new message of the commit the rebase in
    /// progress stopped at, when an interactiveRebase() plan rewords
    /// it; empty otherwise. `hash`, when given, gets that commit's.
    QString stoppedRewordMessage(std::string* hash = nullptr) const;

    /// stoppedRewordMessage() when git hasn't given the commit that
    /// message: nothing is staged, and HEAD still has the commit's old
    /// one (a hook turned the new one down). Empty otherwise.
    QString rejectedRewordMessage() const;

    /// Where the rebase stopped at a reword, puts its new message where
    /// git keeps the message of the commit it stopped at
    /// (rebase-merge/message, and MERGE_MSG at a conflict), for a
    /// `git rebase --continue` or `git commit` in a terminal.
    void keepRewordMessage() const;

    std::string workDir_;
    std::string gitPath_;
    std::vector<std::shared_ptr<std::atomic<bool>>> cancelFlags_;
};

} // namespace gitbolt::git
