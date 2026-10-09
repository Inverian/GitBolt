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
    # QT_FORCE_STDERR_LOGGING: on Windows, QtTest writes its results to
    # stdout only when the process has a console window, and sends them
    # to OutputDebugString otherwise. A CI runner's steps have a console
    # but no window, so a failing test printed nothing at all to ctest.
    # Hangs: QtTest's watchdog (QTEST_FUNCTION_TIMEOUT, default 5 min)
    # aborts a test function that runs too long, printing its name, its
    # run time and, where it can attach gdb or lldb, a stack trace. That
    # only happens if it fires before ctest's TIMEOUT kills the binary,
    # so TIMEOUT leaves room for the functions before it and for the
    # debugger; it also catches hangs outside test functions, such as
    # teardown. These binaries take seconds, no function close to 60 s.
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1;QTEST_FUNCTION_TIMEOUT=60000"
        TIMEOUT 180)
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

# widgets/ tests. The terminal's ConPTY backend only exists in Windows
# builds; elsewhere these tests are a QSKIP each.
gitbolt_add_test(test_conpty_process widgets/TestConPtyProcess.cpp)
gitbolt_add_test(test_terminal_widget widgets/TestTerminalWidget.cpp)
if(WIN32)
    target_link_libraries(test_conpty_process PRIVATE gitbolt_conpty)
    target_link_libraries(test_terminal_widget PRIVATE gitbolt_widgets)
endif()

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
