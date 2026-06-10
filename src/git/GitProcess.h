#pragma once

#include "git/Error.h"

#include <string>
#include <vector>

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

    Result<ProcessOutput> push(const std::string& remote, const std::string& branch, bool force = false) const;
    Result<ProcessOutput> pull(const std::string& remote, const std::string& branch) const;
    Result<ProcessOutput> fetch(const std::string& remote = "", bool prune = false) const;

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
    std::string workDir_;
    std::string gitPath_;
};

} // namespace gitbolt::git
