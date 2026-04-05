#pragma once

#include "gitbolt/core/error.h"

#include <optional>
#include <string>

struct git_repository;

namespace gitbolt::core {

enum class ConfigLevel {
    System,
    Global,
    Local,
};

class Config {
public:
    explicit Config(git_repository* repo);

    Result<std::string> getString(const std::string& key, ConfigLevel level = ConfigLevel::Local) const;
    Result<int64_t> getInt(const std::string& key, ConfigLevel level = ConfigLevel::Local) const;
    Result<bool> getBool(const std::string& key, ConfigLevel level = ConfigLevel::Local) const;

    Result<void> setString(const std::string& key, const std::string& value, ConfigLevel level = ConfigLevel::Local);
    Result<void> setInt(const std::string& key, int64_t value, ConfigLevel level = ConfigLevel::Local);
    Result<void> setBool(const std::string& key, bool value, ConfigLevel level = ConfigLevel::Local);

    Result<void> remove(const std::string& key, ConfigLevel level = ConfigLevel::Local);

    std::optional<std::string> userName() const;
    std::optional<std::string> userEmail() const;

private:
    git_repository* repo_;
};

} // namespace gitbolt::core
