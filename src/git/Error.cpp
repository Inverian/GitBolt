#include "git/Error.h"
#include <git2.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace gitbolt::git {

GitError GitError::fromLibgit2(int error_code) {
    if (error_code == 0) return {};

    const git_error* err = git_error_last();
    std::string msg = err ? err->message : "Unknown libgit2 error";

    GitErrorCode code;
    switch (error_code) {
        case GIT_ENOTFOUND:       code = GitErrorCode::NotFound; break;
        case GIT_EEXISTS:         code = GitErrorCode::Exists; break;
        case GIT_EAMBIGUOUS:      code = GitErrorCode::Ambiguous; break;
        case GIT_ECONFLICT:       code = GitErrorCode::Conflict; break;
        case GIT_ELOCKED:         code = GitErrorCode::Locked; break;
        case GIT_EMODIFIED:       code = GitErrorCode::Modified; break;
        case GIT_EAUTH:           code = GitErrorCode::Auth; break;
        case GIT_EUNMERGED:       code = GitErrorCode::Unmerged; break;
        case GIT_ENONFASTFORWARD: code = GitErrorCode::NonFastForward; break;
        case GIT_EINVALIDSPEC:    code = GitErrorCode::InvalidSpec; break;
        case GIT_EUNCOMMITTED:    code = GitErrorCode::Uncommitted; break;
        case GIT_EDIRECTORY:      code = GitErrorCode::Directory; break;
        case GIT_EEOF:            code = GitErrorCode::Eof; break;
        case GIT_EOWNER:          code = GitErrorCode::Owner; break;
        default:                  code = GitErrorCode::GenericError; break;
    }

    return GitError(code, std::move(msg));
}

GitError GitError::fromProcess(int exit_code, const std::string& stderr_output) {
    if (exit_code == 0) return {};
    return GitError(GitErrorCode::ProcessFailed,
                    "Git process exited with code " + std::to_string(exit_code) + ": " + stderr_output);
}

std::string ownerSafeDirectory(const std::string& message) {
    // "repository path '<path>' is not owned by current user": <path> is
    // the first of the work tree, its .git and the git dir that another
    // user owns. safe.directory takes the work tree, which a git dir
    // elsewhere (a worktree's or submodule's, in .git/worktrees or
    // .git/modules) doesn't name.
    const auto open = message.find('\'');
    const auto close = message.rfind('\'');
    if (open == std::string::npos || close <= open + 1)
        return {};
    std::string path = message.substr(open + 1, close - open - 1);
    const std::string gitDir = "/.git";
    if (path.size() > gitDir.size()
        && path.compare(path.size() - gitDir.size(), gitDir.size(), gitDir) == 0)
        path.resize(path.size() - gitDir.size());
    if (path.find(gitDir + '/') != std::string::npos)
        return {};
    return path;
}

std::string safeDirectoryCommand(const std::string& dir, bool windows) {
    // Quoted as git quotes it in its own hint, but with double quotes on
    // Windows, where cmd takes no single ones (and no path has a '"').
    // Git for Windows reads a value starting with '/' (a share's
    // //server/share) as under its own folder unless it starts with
    // %(prefix)/, as its own hint has it; libgit2 drops that prefix.
    const std::string value = windows && !dir.empty() && dir.front() == '/' ? "%(prefix)/" + dir : dir;
    const bool plain = !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x80 || (std::isalnum(u) != 0) || std::strchr("/._-+:@", c) != nullptr;
    });
    std::string quoted;
    if (plain) {
        quoted = value;
    } else if (windows) {
        quoted = '"' + value + '"';
    } else {
        quoted = "'";
        for (const char c : value)
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        quoted += '\'';
    }
    return "git config --global --add safe.directory " + quoted;
}

} // namespace gitbolt::git
