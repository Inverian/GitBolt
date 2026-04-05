#pragma once

#include "git/ObjectId.h"

#include <string>
#include <vector>

namespace gitbolt::git {

enum class MergePreference {
    Normal,
    FastForwardOnly,
    NoFastForward,
};

enum class MergeAnalysis {
    Normal,
    UpToDate,
    FastForward,
    Unborn,
};

struct MergeConflictEntry {
    std::string path;
    std::string ancestorContent;
    std::string oursContent;
    std::string theirsContent;
    ObjectId ancestorId;
    ObjectId oursId;
    ObjectId theirsId;
};

struct MergeResult {
    MergeAnalysis analysis;
    bool hasConflicts = false;
    std::vector<MergeConflictEntry> conflicts;
    ObjectId resultTreeId;
};

} // namespace gitbolt::git
