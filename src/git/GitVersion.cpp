#include "git/GitVersion.h"
#include "git/GitProcess.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <mutex>
#include <tuple>

namespace gitbolt::git {

namespace {

// How long `git --version` may take: Apple's /usr/bin/git looks up the
// developer tools first, and a virus scanner may hold up the first run
// of Git for Windows.
constexpr int kVersionTimeoutMs = 10000;

// What the last probeGit() found, for lastGitProbe().
std::mutex probeMutex;
std::optional<GitProbe> lastProbe;

#ifdef Q_OS_MACOS
// /usr/bin/git is a stand-in that runs the git of the developer tools
// xcode-select picked, and asks to install the Command Line Tools when
// there are none: a window that a version check, at launch, had no
// business opening. Any of the places it would look will do.
bool appleDeveloperToolsInstalled() {
    QStringList dirs;
    if (const QString chosen = qEnvironmentVariable("DEVELOPER_DIR"); !chosen.isEmpty())
        dirs << chosen;
    if (const QString selected =
            QFileInfo(QStringLiteral("/var/db/xcode_select_link")).symLinkTarget();
        !selected.isEmpty())
        dirs << selected;
    dirs << QStringLiteral("/Library/Developer/CommandLineTools")
         << QStringLiteral("/Applications/Xcode.app/Contents/Developer");
    return std::any_of(dirs.cbegin(), dirs.cend(), [](const QString& dir) {
        return QFileInfo(dir + QStringLiteral("/usr/bin/git")).isExecutable();
    });
}
#endif

} // namespace

std::optional<GitVersion> parseGitVersion(const QString& output) {
    static const QRegularExpression line(
        QStringLiteral(R"(^git version ((\d+)\.(\d+)(?:\.(\d+))?(.*))$)"));
    // "(Apple Git-154)", after a space.
    static const QRegularExpression apple(QStringLiteral(R"(^ +\(Apple Git-\d+[^)]*\)$)"));
    // ".windows.3", after a release candidate's "-rc2" or ".rc2".
    static const QRegularExpression windows(QStringLiteral(R"(^(?:[.-]rc\d+)?\.windows\.(\d+)$)"));
    // Nothing, a release candidate, or a build from a checkout:
    // "2.52.0.153.g1a2b3c4", with ".dirty" if it had changes.
    static const QRegularExpression upstream(
        QStringLiteral(R"(^(?:[.-]rc\d+)?(?:\.\d+\.g[0-9a-f]+)?(?:\.dirty)?$)"));

    const QString first = output.section(QLatin1Char('\n'), 0, 0).trimmed();
    const QRegularExpressionMatch match = line.match(first);
    if (!match.hasMatch())
        return std::nullopt;
    GitVersion version;
    bool ok = false;
    version.major = match.captured(2).toInt(&ok);
    if (!ok)
        return std::nullopt;
    version.minor = match.captured(3).toInt(&ok);
    if (!ok)
        return std::nullopt;
    if (match.hasCaptured(4)) {
        version.patch = match.captured(4).toInt(&ok);
        if (!ok)
            return std::nullopt;
    }
    version.text = match.captured(1);
    const QString rest = match.captured(5);
    if (apple.match(rest).hasMatch()) {
        version.build = GitVersion::Build::Apple;
    } else if (const auto w = windows.match(rest); w.hasMatch()) {
        version.build = GitVersion::Build::Windows;
        version.windowsRevision = w.captured(1).toInt();
    } else if (upstream.match(rest).hasMatch()) {
        version.build = GitVersion::Build::Upstream;
    } else {
        version.build = GitVersion::Build::Other;
    }
    return version;
}

int compareGitVersions(const GitVersion& a, const GitVersion& b) {
    const auto key = [](const GitVersion& v) {
        return std::tie(v.major, v.minor, v.patch, v.windowsRevision);
    };
    if (key(a) < key(b))
        return -1;
    return key(b) < key(a) ? 1 : 0;
}

// check-ci-pins: git/git advisories reviewed 2026-10-24
// check-ci-pins: git-for-windows/git advisories reviewed 2026-10-24
// check-ci-pins: git-lfs/git-lfs advisories reviewed 2026-10-24
//
// Advisories rated high or critical whose attacker is a repository the
// user clones or opens, or a server they fetch from, with git's default
// settings.
const GitAdvisories& builtInGitAdvisories() {
    using Build = GitVersion::Build;
    using Risk = GitAdvisory::Risk;
    static const GitAdvisories table{
        // 2.25 added git sparse-checkout, 2.24 the --end-of-options
        // GitBolt's commands rely on.
        {2, 25, 0},
        {
            // Git LFS 3.0.0 to 3.8.0, in Git for Windows' full installer,
            // runs commands a repository's config or an SSH URL names;
            // 2.56.0(3) ships 3.8.1.
            {{"CVE-2026-105803", "CVE-2026-105802", "CVE-2026-69260", "CVE-2026-69261"},
             Risk::RunsCode, {Build::Windows}, {{2, 56, 0, 3}}},
            // A cloned symbolic link to a network share sends it the NTLM hash.
            {{"CVE-2026-32631"}, Risk::LeaksCredentials, {Build::Windows}, {{2, 53, 0, 3}}},
            // Cloning from a malicious server sends it the NTLM hash.
            {{"CVE-2025-66413"}, Risk::LeaksCredentials, {Build::Windows},
             {{2, 47, 3, 2}, {2, 51, 2, 2}, {2, 52, 0, 2}, {2, 53, 0, 2}}},
            // A recursive clone runs a submodule's hook (48384, in CISA's
            // KEV catalog), a bundle URI writes files anywhere, gitk runs a
            // repository's script. Apple's fix: 2.50.1 (Apple Git-155).
            {{"CVE-2025-48384", "CVE-2025-48385", "CVE-2025-27614"}, Risk::RunsCode,
             {Build::Upstream, Build::Apple, Build::Windows},
             {{2, 43, 7}, {2, 44, 4}, {2, 45, 4}, {2, 46, 4}, {2, 47, 3}, {2, 48, 2}, {2, 49, 1},
              {2, 50, 1}}},
            // A recursive clone on a case-insensitive file system runs a hook.
            {{"CVE-2024-32002", "CVE-2024-32004", "CVE-2024-32465"}, Risk::RunsCode,
             {Build::Upstream, Build::Apple, Build::Windows},
             {{2, 39, 4}, {2, 40, 2}, {2, 41, 1}, {2, 42, 2}, {2, 43, 4}, {2, 44, 1}, {2, 45, 1}}},
            // Not warned about:
            // CVE-2026-62960: needs transfer.bundleURI=true, off by default.
            // CVE-2025-26625, CVE-2024-53263: older Git LFS, covered above.
            // CVE-2025-48386: medium (wincred), fixed with CVE-2025-48384.
            // CVE-2024-52005: a server's messages reach a terminal unfiltered;
            //   GitBolt shows them as plain text.
            // GHSA-rxqw-wxqg-g7hw, CVE-2026-100407, CVE-2026-100408: medium
            //   (wincred, two bytes of heap, no code execution shown).
            // CVE-2023-29012, CVE-2023-29011 (Git for Windows 2.40.1),
            //   CVE-2023-23618 (its gitk in 2.39.2): older than every
            //   Git for Windows fix above.
            // Low: CVE-2024-52006, CVE-2024-50349, CVE-2024-32020,
            //   CVE-2024-32021, CVE-2025-27613 (gitk), CVE-2023-25815.
        }};
    return table;
}

bool isAffected(const GitVersion& version, const GitAdvisory& advisory) {
    if (!advisory.builds.contains(version.build))
        return false;
    const auto sameLine = [&version](const GitVersion& fixed) {
        return fixed.major == version.major && fixed.minor == version.minor;
    };
    if (const auto own = std::find_if(advisory.fixed.cbegin(), advisory.fixed.cend(), sameLine);
        own != advisory.fixed.cend())
        return compareGitVersions(version, *own) < 0;
    // A line that got no fix of its own: affected if it is older than
    // the newest line that did (it was left out, or had ended), not if
    // it came after (it started out fixed).
    const auto newest = std::max_element(
        advisory.fixed.cbegin(), advisory.fixed.cend(),
        [](const GitVersion& a, const GitVersion& b) { return compareGitVersions(a, b) < 0; });
    return std::tie(version.major, version.minor) < std::tie(newest->major, newest->minor);
}

GitPlatform currentGitPlatform() {
#if defined(Q_OS_MACOS)
    return GitPlatform::MacOS;
#elif defined(Q_OS_WIN)
    return GitPlatform::Windows;
#else
    return GitPlatform::Other;
#endif
}

GitAssessment assessGitVersion(const GitVersion& version, GitPlatform platform,
                               const GitAdvisories& table) {
    GitAssessment assessment;
    assessment.belowFloor = compareGitVersions(version, table.floor) < 0;
    if (platform == GitPlatform::Other)
        return assessment;
    for (const GitAdvisory& advisory : table.advisories) {
        if (isAffected(version, advisory))
            assessment.advisories.push_back(&advisory);
    }
    return assessment;
}

GitProbe probeGit() {
    GitProbe probe;
    probe.path = QString::fromStdString(GitProcess::findGitExecutable());
#ifdef Q_OS_MACOS
    probe.appleToolsMissing = probe.path == QLatin1String("/usr/bin/git")
                              && !appleDeveloperToolsInstalled();
#endif
    if (!probe.path.isEmpty() && !probe.appleToolsMissing) {
        // In the home folder: no repository's config comes into it.
        const auto said = GitProcess(QDir::homePath().toStdString())
                              .run({"--version"}, kVersionTimeoutMs);
        if (said.ok() && said->success())
            probe.version = parseGitVersion(QString::fromStdString(said->stdoutData));
    }
    std::lock_guard<std::mutex> lock(probeMutex);
    lastProbe = probe;
    return probe;
}

std::optional<GitProbe> lastGitProbe() {
    std::lock_guard<std::mutex> lock(probeMutex);
    return lastProbe;
}

} // namespace gitbolt::git
