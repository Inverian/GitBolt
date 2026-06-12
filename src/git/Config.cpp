#include "git/Config.h"
#include <git2.h>

namespace gitbolt::git {

Config::Config(git_repository* repo) : repo_(repo) {}

namespace {
// Open a config handle for the requested level. Local (the default)
// hands back the repository's merged config object — writes land in
// .git/config, which is what every existing caller wants. Global and
// System open that level's file specifically, so a caller asking for
// ~/.gitconfig actually touches ~/.gitconfig: the old code accepted
// the parameter and silently ignored it, writing repo-local config
// no matter what was requested.
int openConfig(git_config** out, git_repository* repo,
               ConfigLevel level) {
    git_config* full = nullptr;
    int err = git_repository_config(&full, repo);
    if (err < 0)
        return err;
    if (level == ConfigLevel::Local) {
        *out = full;
        return 0;
    }
    const git_config_level_t lvl = (level == ConfigLevel::System)
        ? GIT_CONFIG_LEVEL_SYSTEM
        : GIT_CONFIG_LEVEL_GLOBAL;
    git_config* leveled = nullptr;
    err = git_config_open_level(&leveled, full, lvl);
    git_config_free(full);
    if (err < 0)
        return err;
    *out = leveled;
    return 0;
}
} // namespace

Result<std::string> Config::getString(const std::string& key, ConfigLevel level) const {
    git_config* cfg = nullptr;
    int err = openConfig(&cfg, repo_, level);
    if (err < 0) return GitError::fromLibgit2(err);

    git_buf buf = GIT_BUF_INIT;
    err = git_config_get_string_buf(&buf, cfg, key.c_str());
    git_config_free(cfg);
    if (err < 0) { git_buf_dispose(&buf); return GitError::fromLibgit2(err); }

    std::string result(buf.ptr, buf.size);
    git_buf_dispose(&buf);
    return result;
}

Result<int64_t> Config::getInt(const std::string& key, ConfigLevel level) const {
    git_config* cfg = nullptr;
    int err = openConfig(&cfg, repo_, level);
    if (err < 0) return GitError::fromLibgit2(err);

    int64_t value = 0;
    err = git_config_get_int64(&value, cfg, key.c_str());
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return value;
}

Result<bool> Config::getBool(const std::string& key, ConfigLevel level) const {
    git_config* cfg = nullptr;
    int err = openConfig(&cfg, repo_, level);
    if (err < 0) return GitError::fromLibgit2(err);

    int value = 0;
    err = git_config_get_bool(&value, cfg, key.c_str());
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return value != 0;
}

Result<void> Config::setString(const std::string& key, const std::string& value, ConfigLevel level) {
    git_config* cfg = nullptr;
    int err = openConfig(&cfg, repo_, level);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_config_set_string(cfg, key.c_str(), value.c_str());
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Config::setInt(const std::string& key, int64_t value, ConfigLevel level) {
    git_config* cfg = nullptr;
    int err = openConfig(&cfg, repo_, level);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_config_set_int64(cfg, key.c_str(), value);
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Config::setBool(const std::string& key, bool value, ConfigLevel level) {
    git_config* cfg = nullptr;
    int err = openConfig(&cfg, repo_, level);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_config_set_bool(cfg, key.c_str(), value ? 1 : 0);
    git_config_free(cfg);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Config::remove(const std::string& key, ConfigLevel level) {
    git_config* cfg = nullptr;
    int err = openConfig(&cfg, repo_, level);
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
