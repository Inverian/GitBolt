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
    /// every process it started. git is interrupted first so it can
    /// roll back its lockfiles: SIGINT (Ctrl-C's) to the group on Unix;
    /// on Windows a Ctrl-C to the hidden console git and its children
    /// share, which works while GitBolt has no console of its own (the
    /// app never does). The group then gets as long again to finish (a
    /// hook putting back what it set aside), and whatever is left is
    /// killed: SIGKILL, or the job ended. Where no Ctrl-C could be
    /// sent, the job is ended outright, and a lockfile git held stays
    /// behind.
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

    /// For a git command nobody is waiting on (the periodic auto-fetch):
    /// a question git or ssh has for the user fails it at once instead
    /// of popping up GitBolt's askpass, which every git child gets
    /// otherwise (see applyEnvironment). git then reports "terminal
    /// prompts disabled", ssh "Host key verification failed." or
    /// "Permission denied". Credential helpers still answer.
    void forbidPrompts() { promptsForbidden_ = true; }

    /// git's variables that say where the repository is, or its config
    /// (`git rev-parse --local-env-vars`, less GIT_CONFIG_PARAMETERS and
    /// GIT_CONFIG_COUNT). No git child, terminal or gitk inherits them
    /// from GitBolt's own environment.
    static const std::vector<std::string>& repositoryEnvVars();

    /// For a bare repository: GIT_DIR names it, rather than git finding
    /// it from the working directory, which git refuses where
    /// safe.bareRepository is "explicit" ("cannot use bare repository").
    void setGitDir(const std::string& gitDir) { gitDir_ = gitDir; }

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

    /// A branch, tag, remote or other ref name, or a revision, that is
    /// to reach git as an operand: refused (GitErrorCode::InvalidSpec,
    /// with a message that names it) where it begins with '-', which
    /// git reads as an option wherever no "--" comes before it. git
    /// allows such names all the same: a fetch keeps the refs a remote
    /// advertises as they are ("origin/-x"), and update-ref makes a
    /// branch of any of them. push, deleteRemoteBranch (of the remote),
    /// pull, fetch, the git-flow commands and the ones that read a
    /// commit id from git's rebase or sequencer state ask this first,
    /// and so do the commands GitService and the window build, whatever
    /// separator they put in front of the name as well; only the ones
    /// that clear such a name away (a remote branch delete, a rename's
    /// old name) leave it to their "--". Empty is fine: the commands
    /// leave an empty one out.
    static Result<void> checkOperand(const std::string& name);

    /// `git push -- <remote> <refspec>`: `branch` as a full ref, so
    /// that git reads it as a refspec and nothing else (a short name
    /// git takes for refs/heads/<branch>, a name under refs/ as it
    /// is). Refused before git runs for a name checkOperand() turns
    /// down. setUpstream adds --set-upstream: publish a branch and
    /// record <remote>/<branch> as its upstream (what `git push -u`
    /// does).
    Result<ProcessOutput> push(const std::string& remote, const std::string& branch,
                               bool force = false, bool setUpstream = false) const;
    /// `git push --delete -- <remote> <branch>`, refused before git
    /// runs for a remote checkOperand() turns down. Not for the branch:
    /// after "--" it is a refspec and nothing else, and a branch named
    /// "-x" on the server is to be deleted as any other.
    Result<ProcessOutput> deleteRemoteBranch(const std::string& remote,
                                             const std::string& branch) const;
    Result<ProcessOutput> pull(const std::string& remote, const std::string& branch) const;
    /// pruneByDefault prunes as fetch.prune=true does, unless the
    /// remote's own remote.<name>.prune says otherwise: every ref the
    /// remote's fetch refspecs write to that the server no longer has,
    /// tags too with pruneTags set. GitService asks for it only where
    /// those are remote-tracking refs alone.
    Result<ProcessOutput> fetch(const std::string& remote = "", bool pruneByDefault = false) const;

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
    /// `git rebase --skip`, refused (GitErrorCode::Uncommitted, with
    /// kSkipWouldDiscard) where it would throw away the user's own work:
    /// at a stop whose commit is made (committedRebaseStop()), git's
    /// --skip resets the work tree to it, and every change to a tracked
    /// file there, staged or not, is the user's, made since.
    Result<ProcessOutput> rebaseSkip() const;
    static constexpr const char* kSkipWouldDiscard =
        "Skipping would discard your uncommitted changes: the commit the rebase "
        "stopped at is made, and git resets the working tree to it. Commit them "
        "(amend the commit, or a commit of their own) or stash them first.";

    /// Whether a tracked file differs from HEAD, in the index or the work
    /// tree (untracked files, and what is inside a submodule, aside).
    /// True when git can't tell: it's asked before throwing work away.
    bool hasTrackedChanges() const;

    /// The todo verb ("pick", "reword" or "edit") of the commit the
    /// rebase in progress stopped at, when that commit is in the branch
    /// already: an `edit` stops once it has picked it (git's
    /// rebase-merge/amend says so, whatever the user has done there
    /// since: an amend, a split), and a reword whose new message a hook
    /// turned down stops with it picked under its old one (see
    /// headIsPickOf(); where that can't be told, the verb too).
    /// rebaseSkip() then only goes on, and the commit stays. Empty where
    /// the stop has nothing committed (a conflict, CHERRY_PICK_HEAD),
    /// and with no rebase in progress.
    std::string committedRebaseStop() const;

    /// `git cherry-pick --continue`, or `git revert --continue` when
    /// `revert`: carries on with a sequence of several commits (see
    /// Repository::sequencePaused()). Skip leaves out the commit the
    /// sequence stopped at first. As long as a rebase step may take.
    Result<ProcessOutput> sequenceContinue(bool revert) const;
    /// Refused (GitErrorCode::Uncommitted, with kSequenceSkipWouldDiscard)
    /// where it would throw away staged work (stagedWorkWithoutPick()).
    Result<ProcessOutput> sequenceSkip(bool revert) const;
    static constexpr const char* kSequenceSkipWouldDiscard =
        "Skipping would discard your staged changes: git stopped before it picked "
        "the commit, and its --skip resets the index to HEAD. Commit them or stash "
        "them first.";

    /// Changes are staged, and no pick or revert is pending
    /// (CHERRY_PICK_HEAD, REVERT_HEAD): a sequence stopped before it
    /// picked the commit, where its --skip and --abort reset the index
    /// to HEAD (`reset --merge`) and the staged changes are the user's.
    /// True when git can't tell: it's asked before throwing work away.
    bool stagedWorkWithoutPick() const;

    /// The commit a paused cherry-pick or revert sequence stopped at,
    /// as its todo names it ("<short id> <subject>"), where git never
    /// picked (or reverted) it, and its --continue would count it done
    /// all the same: no pick is pending (CHERRY_PICK_HEAD, REVERT_HEAD),
    /// and no commit made since the sequence's last own step (its
    /// abort-safety) is that pick. git stops before a pick it refuses
    /// over local changes; a commit of them then flipped the bar to
    /// Continue, which went on from the commit after it. A pick, and a
    /// commit that concludes a conflicted one, keep the commit's author
    /// and date; a revert's message names it ("This reverts commit"),
    /// unless the user's own words replaced git's; a commit() that
    /// concluded the stop counts whatever its message, while it is
    /// still there. Empty where the pick was made, with no sequence,
    /// and where git can't tell.
    std::string unpickedSequenceStop() const;

    /// `git commit -F -` (`--amend` too when `amend`), the message on
    /// stdin: the commit a terminal's git would make. The repository's
    /// hooks run (core.hooksPath honoured), commit.gpgSign and
    /// commit.cleanup apply, and git concludes a merge, cherry-pick or
    /// revert in progress, a merge's autostash included. Up to five
    /// minutes, for slow hooks. git runs in a process group of its own,
    /// so a hook that hangs dies with it when that time is up or a
    /// cancel flag is set.
    ///
    /// Once it is made: where it concludes the stop of a cherry-pick or
    /// revert of several commits, that is noted in git's sequencer
    /// folder, for unpickedSequenceStop(), and so is an amend of that
    /// commit, which replaces it; where a rebase stopped at a reword of
    /// an interactiveRebase() plan, the plan's message for it goes, the
    /// user's commit there standing (rebaseContinue() no longer tries
    /// it again).
    Result<ProcessOutput> commit(const std::string& message, bool amend = false) const;

    /// The repository has a prepare-commit-msg hook that git would run,
    /// where git looks for it (core.hooksPath honoured): an executable
    /// file on Unix; on Windows any file, through sh, or the name with
    /// ".exe" (a compiled hook), which Git for Windows tries too.
    bool hasPrepareCommitMsgHook() const;

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
    /// GIT_EDITOR that never waits on anyone (see kEditor). With
    /// `allowPrompts`, GitBolt is git's and ssh's askpass; without,
    /// nothing asks (forbidPrompts()).
    static void applyEnvironment(QProcess& process, bool allowPrompts = true);

    /// What run(), runWithInput() and interactiveRebase() share: git
    /// in a process tree, fed `stdinData` when given, with `extraEnv`
    /// on top of applyEnvironment()'s.
    Result<ProcessOutput> runImpl(const std::vector<std::string>& args, int timeoutMs,
                                  const std::string* stdinData = nullptr,
                                  const std::map<std::string, std::string>& extraEnv = {}) const;
    /// Whether any of cancelFlags_ is set.
    bool cancelled() const;

    /// The -c options for a git that carries the rebase in progress on:
    /// kUtf8Commits, and "-c core.commentChar=<c>" with the comment
    /// character that interactiveRebase() started it with (a reword that
    /// stopped on a conflict is committed then). No comment character
    /// for a rebase started some other way, which keeps the user's own
    /// setting.
    std::vector<std::string> rebaseConfig() const;

    /// "-c i18n.commitEncoding=UTF-8", for every git that writes a
    /// message of GitBolt's into a commit: they are UTF-8 (Qt's
    /// strings), and git labels a commit's message with the configured
    /// encoding without converting it. In a repository set to
    /// ISO-8859-1 "Café" went into history labelled as Latin-1, and read
    /// "CafÃ©" everywhere that honours the label. With UTF-8 there is no
    /// label at all, as libgit2's commits had none.
    static constexpr const char* kUtf8Commits = "i18n.commitEncoding=UTF-8";

    /// `git rev-parse --git-path <path>`, made absolute; empty when git
    /// can't say (no repository).
    QString gitPath(const QString& path) const;

    /// The file with the new message of the commit the rebase in
    /// progress stopped at, when an interactiveRebase() plan rewords
    /// it; empty otherwise. `hash`, when given, gets that commit's.
    QString stoppedRewordMessage(std::string* hash = nullptr) const;

    /// stoppedRewordMessage() when git hasn't given the commit that
    /// message: nothing is staged, no pick is pending (CHERRY_PICK_HEAD),
    /// and HEAD is that commit as git picked it, under its old message
    /// (a hook turned the new one down; see headIsPickOf()), with
    /// nothing committed since, as far as headIsTheRewordStop() can
    /// tell. Empty otherwise.
    QString rejectedRewordMessage() const;

    /// Whether HEAD is the commit `hash` as a rebase picked it: that
    /// commit, or one with its author, date and patch-id (without
    /// context lines, so a pick onto a base that changed nearby lines
    /// still counts). Not its message, which a prepare-commit-msg hook
    /// may have added to; a commit with no change at all has to start
    /// with it. nullopt where git couldn't say (a command failed or
    /// timed out).
    std::optional<bool> headIsPickOf(const std::string& hash) const;

    /// HEAD is still what the stop at the reword of `hash` left, not a
    /// commit of the user's since, such as an amend of the message: that
    /// commit (a fast-forward); else, where git noted HEAD at the stop
    /// (rebase-merge/amend), that HEAD; else the newest entry of HEAD's
    /// reflog is git's pick for the reword ("rebase (reword): ...");
    /// else, where HEAD keeps no reflog, HEAD's message is the stopped
    /// commit's old one, or starts with it where a prepare-commit-msg
    /// hook (hasPrepareCommitMsgHook()) may have added to git's pick.
    /// An amend that keeps the old message, with a hook like that, can
    /// still pass for the pick there; one made with commit() can't (it
    /// removes the plan's message).
    bool headIsTheRewordStop(const std::string& hash) const;

    /// Where the rebase stopped at a reword, puts its new message where
    /// git keeps the message of the commit it stopped at
    /// (rebase-merge/message, and MERGE_MSG at a conflict), for a
    /// `git rebase --continue` or `git commit` in a terminal.
    void keepRewordMessage() const;

    std::string workDir_;
    std::string gitDir_;   // GIT_DIR, if set (setGitDir)
    std::string gitPath_;
    std::vector<std::shared_ptr<std::atomic<bool>>> cancelFlags_;
    bool promptsForbidden_ = false;
};

} // namespace gitbolt::git
