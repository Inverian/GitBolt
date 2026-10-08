# GitBolt test suite
#
# Each test file has its own QTEST_MAIN and compiles to its own executable.
# CTest then runs them all as separate tests so one failure doesn't block
# the rest.
#
# Helper function to reduce boilerplate: gitbolt_add_test(<name> <source>)
# creates an executable, links the libraries it needs, and registers it
# with CTest.

function(gitbolt_add_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE
        gitbolt_git
        gitbolt_models
        gitbolt_util
        Qt6::Test
        Qt6::Widgets  # models/ link Qt6::Widgets via Qt::Gui for QFont/QIcon
    )
    gitbolt_set_warnings(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# git/ core tests — exercise the libgit2 wrapper against a temporary repo
gitbolt_add_test(test_repository     git/TestRepository.cpp)
gitbolt_add_test(test_revwalk        git/TestRevWalk.cpp)
gitbolt_add_test(test_diff           git/TestDiff.cpp)
gitbolt_add_test(test_status         git/TestStatus.cpp)

# models/ tests — exercise the Qt model layer (the log model from in-memory
# data, the branch model from what a temp repo reports)
gitbolt_add_test(test_commit_log_model models/TestCommitLogModel.cpp)
gitbolt_add_test(test_branch_model     models/TestBranchModel.cpp)

# services/ tests — GitService orchestration against real temp repos.
# Needs the services lib on top of the defaults the helper links.
gitbolt_add_test(test_git_service services/TestGitService.cpp)
target_link_libraries(test_git_service PRIVATE gitbolt_services)

# app/ tests — CHANGELOG.md as Help → Changelog shows it. Built with the
# app's own .qrc so it reads the embedded text, not just the source file.
gitbolt_add_test(test_changelog app/TestChangelog.cpp)
target_sources(test_changelog PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app/resources/gitbolt.qrc)
target_compile_definitions(test_changelog PRIVATE
    GITBOLT_VERSION="${PROJECT_VERSION}"
    GITBOLT_CHANGELOG_PATH="${PROJECT_SOURCE_DIR}/CHANGELOG.md")

# e2e/ tests — the real app binary driven through the test bridge.
add_subdirectory(e2e)
