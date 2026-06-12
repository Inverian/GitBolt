#include "git/GitProcess.h"
#include "git/GitProcessLog.h"
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <sstream>

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

Result<ProcessOutput> GitProcess::push(const std::string& remote, const std::string& branch, bool force) const {
    // git is strict about empty refspecs: `git push origin ""` errors
    // with "fatal: invalid refspec ''" instead of doing the sensible
    // thing (push the current branch to its upstream). Drop empty
    // arguments so the toolbar's "Push" with no specific branch hits
    // the bare `git push origin` form, which honors push.default.
    std::vector<std::string> args = {"push"};
    if (force) args.emplace_back("--force-with-lease");
    if (!remote.empty()) args.push_back(remote);
    if (!branch.empty()) args.push_back(branch);
    return run(args, 120000);
}

Result<ProcessOutput> GitProcess::pull(const std::string& remote, const std::string& branch) const {
    // Same empty-arg avoidance as push — `git pull origin ""` errors
    // out, but bare `git pull origin` (or just `git pull`) honors the
    // tracking branch.
    std::vector<std::string> args = {"pull"};
    if (!remote.empty()) args.push_back(remote);
    if (!branch.empty()) args.push_back(branch);
    return run(args, 120000);
}

Result<ProcessOutput> GitProcess::fetch(const std::string& remote, bool prune) const {
    std::vector<std::string> args = {"fetch"};
    if (!remote.empty()) args.push_back(remote);
    if (prune) args.emplace_back("--prune");
    return run(args, 120000);
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
