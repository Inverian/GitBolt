add_library(gitbolt_core STATIC
    src/error.cpp
    src/object_id.cpp
    src/signature.cpp
    src/repository.cpp
    src/commit.cpp
    src/diff.cpp
    src/branch.cpp
    src/remote.cpp
    src/index.cpp
    src/status.cpp
    src/blame.cpp
    src/stash.cpp
    src/tag.cpp
    src/submodule.cpp
    src/worktree.cpp
    src/config.cpp
    src/revwalk.cpp
    src/merge.cpp
    src/rebase.cpp
    src/cherrypick.cpp
    src/git_process.cpp
)

target_include_directories(gitbolt_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Link libgit2
if(TARGET PkgConfig::LIBGIT2)
    target_link_libraries(gitbolt_core PUBLIC PkgConfig::LIBGIT2)
elseif(TARGET libgit2::git2)
    target_link_libraries(gitbolt_core PUBLIC libgit2::git2)
elseif(TARGET git2)
    target_link_libraries(gitbolt_core PUBLIC git2)
endif()

target_link_libraries(gitbolt_core PRIVATE Qt6::Core)

gitbolt_set_warnings(gitbolt_core)
