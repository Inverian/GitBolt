#include "git/GitVersion.h"
#include "git/GitProcess.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>

namespace gitbolt::git {

namespace {

// How long `git --version` may take: Apple's /usr/bin/git looks up the
// developer tools first, and a virus scanner may hold up the first run
// of Git for Windows.
constexpr int kVersionTimeoutMs = 10000;

std::optional<GitVersion::Build> buildNamed(const QString& name) {
    if (name == QLatin1String("upstream"))
        return GitVersion::Build::Upstream;
    if (name == QLatin1String("apple"))
        return GitVersion::Build::Apple;
    if (name == QLatin1String("windows"))
        return GitVersion::Build::Windows;
    return std::nullopt;
}

// A version as git-advisories.json writes one: "2.43.7",
// "2.53.0.windows.4".
std::optional<GitVersion> versionNamed(const QString& name) {
    return parseGitVersion(QStringLiteral("git version ") + name);
}

// What probeGit() found, by git.
struct Probes {
    std::mutex mutex;
    std::map<QString, GitProbe> byPath;
    std::optional<GitProbe> last;
};

Probes& probes() {
    static Probes instance;
    return instance;
}

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
    static const QRegularExpression apple(QStringLiteral(R"(^ +\(Apple Git-(\d+)[^)]*\)$)"));
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
    if (const auto a = apple.match(rest); a.hasMatch()) {
        version.build = GitVersion::Build::Apple;
        version.appleBuild = a.captured(1).toInt();
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

std::optional<GitAdvisories> parseGitAdvisories(const QByteArray& json) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject root = document.object();

    GitAdvisories table;
    table.checked = QDate::fromString(root.value(QStringLiteral("checked")).toString(),
                                      Qt::ISODate);
    const auto floor = versionNamed(root.value(QStringLiteral("floor")).toString());
    if (!table.checked.isValid() || !floor)
        return std::nullopt;
    table.floor = *floor;

    for (const QJsonValue& value : root.value(QStringLiteral("advisories")).toArray()) {
        const QJsonObject entry = value.toObject();
        GitAdvisory advisory;
        for (const QJsonValue& id : entry.value(QStringLiteral("ids")).toArray()) {
            if (id.toString().isEmpty())
                return std::nullopt;
            advisory.ids << id.toString();
        }
        const QString risk = entry.value(QStringLiteral("risk")).toString();
        if (risk == QLatin1String("code"))
            advisory.risk = GitAdvisory::Risk::RunsCode;
        else if (risk == QLatin1String("credentials"))
            advisory.risk = GitAdvisory::Risk::LeaksCredentials;
        else
            return std::nullopt;
        for (const QJsonValue& name : entry.value(QStringLiteral("builds")).toArray()) {
            const auto build = buildNamed(name.toString());
            if (!build)
                return std::nullopt;
            advisory.builds << *build;
        }
        for (const QJsonValue& name : entry.value(QStringLiteral("fixed")).toArray()) {
            const auto fixed = versionNamed(name.toString());
            if (!fixed)
                return std::nullopt;
            // One release per line: which one would count?
            const bool lineListed = std::any_of(
                advisory.fixed.cbegin(), advisory.fixed.cend(), [&](const GitVersion& f) {
                    return f.major == fixed->major && f.minor == fixed->minor;
                });
            if (lineListed)
                return std::nullopt;
            advisory.fixed.push_back(*fixed);
        }
        advisory.appleFixed = entry.value(QStringLiteral("apple_fixed")).toInt(0);
        if (advisory.ids.isEmpty() || advisory.builds.isEmpty() || advisory.fixed.empty())
            return std::nullopt;
        table.advisories.push_back(std::move(advisory));
    }
    if (table.advisories.empty())
        return std::nullopt;
    return table;
}

const GitAdvisories& builtInGitAdvisories() {
    static const GitAdvisories table = [] {
        QFile file(QStringLiteral(":/git/git-advisories.json"));
        if (file.open(QIODevice::ReadOnly)) {
            if (auto parsed = parseGitAdvisories(file.readAll()))
                return std::move(*parsed);
        }
        qWarning("git-advisories.json is missing or malformed: no git version warnings");
        return GitAdvisories{};
    }();
    return table;
}

bool isAffected(const GitVersion& version, const GitAdvisory& advisory) {
    if (!advisory.builds.contains(version.build))
        return false;
    if (version.build == GitVersion::Build::Apple && advisory.appleFixed > 0
        && version.appleBuild >= advisory.appleFixed)
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
    Probes& state = probes();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (const auto known = state.byPath.find(probe.path); known != state.byPath.end()) {
            state.last = known->second;
            return known->second;
        }
    }
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
    std::lock_guard<std::mutex> lock(state.mutex);
    // No git, or the tools to be installed: asked again next time.
    if (!probe.path.isEmpty() && !probe.appleToolsMissing)
        state.byPath[probe.path] = probe;
    state.last = probe;
    return probe;
}

std::optional<GitProbe> lastGitProbe() {
    Probes& state = probes();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.last;
}

} // namespace gitbolt::git
