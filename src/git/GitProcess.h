#pragma once

#include "git/CloneProgress.h"
#include "git/Error.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class QProcess;

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

    Result<ProcessOutput> run(const std::vector<std::string>& args, int timeoutMs = 30000) const;

    /// Like run(), but feeds `stdinData` to the child's stdin and
    /// closes the write channel. Needed for commands that read a
    /// payload from stdin, e.g. `git apply --cached -` with a patch
    /// (hunk-level staging builds patches in memory; writing temp
    /// files just to hand git a few hundred bytes would be noise).
    Result<ProcessOutput> runWithInput(const std::vector<std::string>& args,
                                       const std::string& stdinData,
                                       int timeoutMs = 30000) const;

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
    /// The exception: when the fetch completed and only the checkout
    /// failed, git keeps the repository on purpose, and so does this.
    /// The error is then CheckoutFailed; its message says where the
    /// repository is and ends with git's advice on finishing the
    /// checkout.
    static Result<void> clone(const std::string& url, const std::string& path,
                              const CloneProgressCallback& onProgress = nullptr,
                              const std::shared_ptr<std::atomic<bool>>& cancelFlag = nullptr);

    /// `url` with its credentials masked, for display (the command
    /// log): "https://user:secret@host/…" → "https://user:***@host/…",
    /// and a token pasted as the user name, "https://<token>@host/…",
    /// → "https://***@host/…". An ssh user name ("ssh://git@host/…",
    /// "git@host:path") is no secret and is kept.
    static std::string redactUrl(const std::string& url);

    Result<ProcessOutput> interactiveRebase(const std::string& onto, const std::string& editorScript) const;
    Result<ProcessOutput> rebaseContinue() const;
    Result<ProcessOutput> rebaseAbort() const;
    Result<ProcessOutput> rebaseSkip() const;

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
    /// fast instead of hanging until the timeout kills it.
    static void applyEnvironment(QProcess& process);

    std::string workDir_;
    std::string gitPath_;
};

} // namespace gitbolt::git
