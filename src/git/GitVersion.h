#pragma once

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace gitbolt::git {

/// What `git --version` says, taken apart.
struct GitVersion {
    /// Whose git it is: git's own (git-scm.com, Homebrew, a Linux
    /// distribution's), Apple's (Xcode and its Command Line Tools:
    /// "2.39.5 (Apple Git-154)"), Git for Windows' ("2.53.0.windows.3"),
    /// or someone else's, such as Microsoft's fork ("2.45.2.vfs.0.4").
    enum class Build { Upstream, Apple, Windows, Other };

    int major = 0;
    int minor = 0;
    int patch = 0;
    /// Git for Windows' N in X.Y.Z.windows.N; 0 for any other git.
    int windowsRevision = 0;
    /// Apple's N in "(Apple Git-N)"; 0 for any other git.
    int appleBuild = 0;
    Build build = Build::Upstream;
    /// What git said after "git version ": "2.39.5 (Apple Git-154)".
    QString text;
};

/// `git --version`'s first line taken apart; nullopt when it isn't one
/// ("git version" and at least a major and a minor number).
std::optional<GitVersion> parseGitVersion(const QString& output);

/// Orders by major, minor, patch and then Git for Windows' revision
/// (a git built by anyone else has 0): -1, 0 or 1.
int compareGitVersions(const GitVersion& a, const GitVersion& b);

/// A security advisory GitBolt warns about (git-advisories.json).
struct GitAdvisory {
    /// What a git it affects can let a repository or a server do.
    enum class Risk {
        RunsCode,           // run a program of theirs on the user's computer
        LeaksCredentials,   // get the user's Windows password hash (NTLM)
    };

    /// CVE IDs, the one to name first.
    QStringList ids;
    Risk risk = Risk::RunsCode;
    /// Whose gits it concerns.
    QList<GitVersion::Build> builds;
    /// The first fixed release of each maintenance line that got one.
    std::vector<GitVersion> fixed;
    /// For Apple's builds: the first fixed "Apple Git-N", 0 if Apple
    /// fixed it only with a newer git.
    int appleFixed = 0;
};

/// git-advisories.json.
struct GitAdvisories {
    /// When the advisories were last looked through.
    QDate checked;
    /// The oldest git GitBolt works with. Default: none.
    GitVersion floor;
    std::vector<GitAdvisory> advisories;
};

/// A table in git-advisories.json's form; nullopt when it is malformed:
/// not JSON, no valid "checked" date or "floor", no advisories, or one
/// with no IDs, an unknown risk or build, or a "fixed" release that
/// isn't a version.
std::optional<GitAdvisories> parseGitAdvisories(const QByteArray& json);

/// The table built into GitBolt (:/git/git-advisories.json), read once.
/// Empty, with no floor, if it couldn't be read (test_git_version
/// fails then).
const GitAdvisories& builtInGitAdvisories();

/// Whether `version` is a git `advisory` concerns and is not fixed in:
/// below the fixed release of its line, or on a line that got none and
/// is older than the newest line that did. An Apple build counts as
/// fixed by its git version or by its Apple Git-N.
bool isAffected(const GitVersion& version, const GitAdvisory& advisory);

/// Where GitBolt runs, as far as warnings about git go.
enum class GitPlatform {
    MacOS,
    Windows,
    /// Linux and the BSDs: distributions backport fixes to the git
    /// they ship and keep its version, so a version says nothing about
    /// them.
    Other,
};

/// The platform this GitBolt was built for.
GitPlatform currentGitPlatform();

/// What GitBolt has to say about a git.
struct GitAssessment {
    /// Older than the table's floor: some of GitBolt's commands fail.
    bool belowFloor = false;
    /// The advisories it is affected by, in the table's order. Always
    /// none on GitPlatform::Other.
    std::vector<const GitAdvisory*> advisories;

    bool ok() const { return !belowFloor && advisories.empty(); }
};

/// `version` on `platform`, by `table`, whose advisories it must
/// outlive.
GitAssessment assessGitVersion(const GitVersion& version, GitPlatform platform,
                               const GitAdvisories& table = builtInGitAdvisories());

/// The git GitBolt runs, and what it says it is.
struct GitProbe {
    /// GitProcess::findGitExecutable(): empty when there is no git.
    QString path;
    /// nullopt when `git --version` failed, said something else, or
    /// wasn't run.
    std::optional<GitVersion> version;
    /// macOS: `path` is /usr/bin/git, Apple's stand-in that runs the
    /// git of Xcode or the Command Line Tools, and neither is
    /// installed. It isn't run: it would ask to install them.
    bool appleToolsMissing = false;
};

/// Finds git and asks it its version, once per git: a later call that
/// finds the same one answers from memory. Waits for the login shell's
/// PATH (util::pathForChildren()) and for git, so never on the GUI
/// thread: MainWindow::checkGitVersion() runs it on a worker.
GitProbe probeGit();

/// What the last probeGit() found, without waiting; nullopt until one
/// has finished.
std::optional<GitProbe> lastGitProbe();

} // namespace gitbolt::git
