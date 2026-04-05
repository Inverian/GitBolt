#pragma once

#include "gitbolt/core/object_id.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gitbolt::core {

enum class DiffLineType {
    Context,
    Addition,
    Deletion,
    ContextEOFNL,
    AddEOFNL,
    DelEOFNL,
    FileHeader,
    HunkHeader,
    Binary,
};

struct DiffLine {
    DiffLineType type;
    std::string content;
    int oldLineno = -1;
    int newLineno = -1;
};

struct DiffHunk {
    std::string header;
    int oldStart = 0;
    int oldLines = 0;
    int newStart = 0;
    int newLines = 0;
    std::vector<DiffLine> lines;
};

enum class DiffStatus {
    Unmodified,
    Added,
    Deleted,
    Modified,
    Renamed,
    Copied,
    Ignored,
    Untracked,
    TypeChanged,
    Unreadable,
    Conflicted,
};

struct DiffFileEntry {
    DiffStatus status;
    std::string oldPath;
    std::string newPath;
    ObjectId oldId;
    ObjectId newId;
    uint16_t similarity = 0;
    bool isBinary = false;
    std::vector<DiffHunk> hunks;

    const std::string& path() const {
        return newPath.empty() ? oldPath : newPath;
    }
};

struct DiffResult {
    std::vector<DiffFileEntry> files;
    size_t totalAdditions = 0;
    size_t totalDeletions = 0;
};

} // namespace gitbolt::core
