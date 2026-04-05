#pragma once

#include "gitbolt/core/commit.h"
#include "gitbolt/core/error.h"

#include <functional>
#include <vector>

struct git_repository;

namespace gitbolt::core {

enum class SortOrder {
    None,
    Topological,
    Time,
    TopologicalTime,
    Reverse,
};

class RevWalk {
public:
    explicit RevWalk(git_repository* repo);
    ~RevWalk();

    RevWalk(const RevWalk&) = delete;
    RevWalk& operator=(const RevWalk&) = delete;
    RevWalk(RevWalk&& other) noexcept;
    RevWalk& operator=(RevWalk&& other) noexcept;

    Result<void> pushHead();
    Result<void> pushRef(const std::string& refname);
    Result<void> push(const ObjectId& id);
    Result<void> hide(const ObjectId& id);
    Result<void> hideRef(const std::string& refname);

    void setSorting(SortOrder order);
    void setFirstParentOnly(bool firstParent);
    void reset();

    Result<std::vector<CommitData>> next(size_t count);
    Result<std::vector<CommitData>> all();

    using WalkCallback = std::function<bool(const CommitData&)>;
    Result<void> walk(WalkCallback callback);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace gitbolt::core
