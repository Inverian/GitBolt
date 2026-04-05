#pragma once

#include "git/ObjectId.h"
#include "git/Signature.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gitbolt::git {

struct BlameHunk {
    ObjectId commitId;
    ObjectId origCommitId;
    Signature signature;
    std::string origPath;
    uint32_t startLine;
    uint32_t lineCount;
    bool boundary = false;
};

struct BlameResult {
    std::string path;
    std::vector<BlameHunk> hunks;
    std::vector<std::string> lines;
};

} // namespace gitbolt::git
