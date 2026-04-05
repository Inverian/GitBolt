#include "gitbolt/core/error.h"
#include <git2.h>

namespace gitbolt::core {

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
        default:                  code = GitErrorCode::GenericError; break;
    }

    return GitError(code, std::move(msg));
}

GitError GitError::fromProcess(int exit_code, const std::string& stderr_output) {
    if (exit_code == 0) return {};
    return GitError(GitErrorCode::ProcessFailed,
                    "Git process exited with code " + std::to_string(exit_code) + ": " + stderr_output);
}

} // namespace gitbolt::core
