#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct git_signature;

namespace gitbolt::git {

struct Signature {
    std::string name;
    std::string email;
    std::chrono::system_clock::time_point when;
    int offset_minutes = 0;

    static Signature fromGit(const git_signature* sig);
};

} // namespace gitbolt::git
