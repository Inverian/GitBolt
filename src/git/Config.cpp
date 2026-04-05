#include "git/Config.h"
#include <git2.h>

namespace gitbolt::git {

Config::Config(git_repository* repo) : repo_(repo) {}

Result<std::string> Config::getString(const std::string& key, ConfigLevel /*level*/) const {
    git_config* cfg = nullptr;
    int err = git_repository_config(&cfg, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    git_buf buf = GIT_BUF_INIT;
    err = git_config_get_string_buf(&buf, cfg, key.c_str());
    git_config_free(cfg);
    if (err < 0) { git_buf_dispose(&buf); return GitError::fromLibgit2(err); }

    std::string result(buf.ptr, buf.size);
    git_buf_dispose(&buf);
    return result;
}

Result<int64_t> Config::getInt(const std::string& key, ConfigLevel /*level*/) const {
    git_config* cfg = nullptr;
    int err = git_repository_config(&cfg, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    int64_t value = 0;
    err = git_config_get_int64(&value, cfg, key.c_str());
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return value;
}

Result<bool> Config::getBool(const std::string& key, ConfigLevel /*level*/) const {
    git_config* cfg = nullptr;
    int err = git_repository_config(&cfg, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    int value = 0;
    err = git_config_get_bool(&value, cfg, key.c_str());
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return value != 0;
}

Result<void> Config::setString(const std::string& key, const std::string& value, ConfigLevel /*level*/) {
    git_config* cfg = nullptr;
    int err = git_repository_config(&cfg, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_config_set_string(cfg, key.c_str(), value.c_str());
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Config::setInt(const std::string& key, int64_t value, ConfigLevel /*level*/) {
    git_config* cfg = nullptr;
    int err = git_repository_config(&cfg, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_config_set_int64(cfg, key.c_str(), value);
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Config::setBool(const std::string& key, bool value, ConfigLevel /*level*/) {
    git_config* cfg = nullptr;
    int err = git_repository_config(&cfg, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_config_set_bool(cfg, key.c_str(), value ? 1 : 0);
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Config::remove(const std::string& key, ConfigLevel /*level*/) {
    git_config* cfg = nullptr;
    int err = git_repository_config(&cfg, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_config_delete_entry(cfg, key.c_str());
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

std::optional<std::string> Config::userName() const {
    auto r = getString("user.name");
    if (r.ok()) return r.value();
    return std::nullopt;
}

std::optional<std::string> Config::userEmail() const {
    auto r = getString("user.email");
    if (r.ok()) return r.value();
    return std::nullopt;
}

} // namespace gitbolt::git
