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
    # DO_NOT_TRACK, GITBOLT_TELEMETRY: belt and braces on top of the
    # facts that no test binary links the app, where telemetry
    # endpoints live, and that the telemetry tests inject the gate's
    # environment instead of reading this one.
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1;QTEST_FUNCTION_TIMEOUT=60000;DO_NOT_TRACK=1;GITBOLT_TELEMETRY=0"
        TIMEOUT 180)
endfunction()

# git/ core tests — exercise the libgit2 wrapper against a temporary repo
gitbolt_add_test(test_repository     git/TestRepository.cpp)
gitbolt_add_test(test_revwalk        git/TestRevWalk.cpp)
gitbolt_add_test(test_diff           git/TestDiff.cpp)
gitbolt_add_test(test_status         git/TestStatus.cpp)
# GitProcess::run(): a timeout stops git's whole process group, gently
# first (Unix; a QSKIP on Windows, where git runs in a job object)
gitbolt_add_test(test_git_process    git/TestGitProcess.cpp)

# Clone through the git CLI. The cancel test clones from a "server"
# that never answers — StallTransport, run by git via ext:: — whose
# path is baked into the test.
gitbolt_add_test(test_clone          git/TestClone.cpp)
add_executable(gitbolt_stall_transport git/StallTransport.cpp)
target_link_libraries(gitbolt_stall_transport PRIVATE Qt6::Core)
gitbolt_set_warnings(gitbolt_stall_transport)
add_dependencies(test_clone gitbolt_stall_transport)
target_compile_definitions(test_clone PRIVATE
    GITBOLT_TEST_STALL_TRANSPORT="$<TARGET_FILE:gitbolt_stall_transport>")

# The linked libgit2 is thread-safe (and logs its version and backends);
# the bundled build is also checked against its pin (cmake/Libgit2.cmake)
gitbolt_add_test(test_libgit2_features git/TestLibgit2Features.cpp)
if(GITBOLT_LIBGIT2_PROVIDER STREQUAL "bundled")
    target_compile_definitions(test_libgit2_features PRIVATE
        GITBOLT_BUNDLED_LIBGIT2_VERSION="${GITBOLT_LIBGIT2_VERSION}")
endif()

# util/ tests — AsyncRunner's threading guarantees (no repository needed)
gitbolt_add_test(test_async_runner util/TestAsyncRunner.cpp)

# telemetry/ — the client against a loopback HTTP stub (HttpStub.h).
# Never a real endpoint: test binaries don't link src/app, where the
# endpoints live.
gitbolt_add_test(test_telemetry telemetry/TestTelemetry.cpp)
target_sources(test_telemetry PRIVATE telemetry/HttpStub.h telemetry/CrashHeader.h)
target_link_libraries(test_telemetry PRIVATE gitbolt_telemetry Qt6::Network)
# Crash reports: fixtures for every refusal, the renderHeader round
# trip, and (POSIX) a death test that parses the crash.log a real
# handler wrote. Its helper is built as "gitbolt", in a directory of
# its own, so the parser counts its frames as the app's.
gitbolt_add_test(test_crash_log_parser telemetry/TestCrashLogParser.cpp)
target_sources(test_crash_log_parser PRIVATE telemetry/CrashHeader.h)
target_link_libraries(test_crash_log_parser PRIVATE gitbolt_telemetry)
target_compile_definitions(test_crash_log_parser PRIVATE
    GITBOLT_CRASH_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/crash")
if(NOT WIN32)
    add_executable(gitbolt_crash_helper telemetry/CrashHelper.cpp)
    target_link_libraries(gitbolt_crash_helper PRIVATE gitbolt_util)
    set_target_properties(gitbolt_crash_helper PROPERTIES
        OUTPUT_NAME gitbolt
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/crash-helper")
    gitbolt_set_warnings(gitbolt_crash_helper)
    add_dependencies(test_crash_log_parser gitbolt_crash_helper)
    target_compile_definitions(test_crash_log_parser PRIVATE
        GITBOLT_CRASH_HELPER="$<TARGET_FILE:gitbolt_crash_helper>")
endif()

# models/ tests — exercise the Qt model layer (the log model from in-memory
# data, the branch model from what a temp repo reports)
gitbolt_add_test(test_commit_log_model models/TestCommitLogModel.cpp)
gitbolt_add_test(test_branch_model     models/TestBranchModel.cpp)

# services/ tests — GitService orchestration against real temp repos.
# Needs the services lib on top of the defaults the helper links, and
# StallTransport for cancelling a fetch that never ends.
gitbolt_add_test(test_git_service services/TestGitService.cpp)
target_link_libraries(test_git_service PRIVATE gitbolt_services)
add_dependencies(test_git_service gitbolt_stall_transport)
target_compile_definitions(test_git_service PRIVATE
    GITBOLT_TEST_STALL_TRANSPORT="$<TARGET_FILE:gitbolt_stall_transport>")

# widgets/ tests. The terminal runs its shell under ConPTY on Windows
# and under a PTY elsewhere. Each backend's end-to-end tests are a QSKIP
# on the other platforms (so all of TestConPtyProcess is, off Windows),
# and the terminal's output path is tested on its own everywhere.
gitbolt_add_test(test_conpty_process widgets/TestConPtyProcess.cpp)
gitbolt_add_test(test_terminal_widget widgets/TestTerminalWidget.cpp)
target_link_libraries(test_terminal_widget PRIVATE gitbolt_widgets)
if(WIN32)
    target_link_libraries(test_conpty_process PRIVATE gitbolt_conpty)
endif()
# The inspector's file tree, off screen. A regression of its context-menu
# test reads memory the tree's proxy model has freed: Guard Malloc
# (macOS) turns that into a crash every time; MALLOC_PERTURB_ (glibc)
# fills it with garbage, to the same end.
gitbolt_add_test(test_file_tree widgets/TestFileTree.cpp)
target_link_libraries(test_file_tree PRIVATE gitbolt_widgets gitbolt_services)
set_property(TEST test_file_tree APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(APPLE)
    # dyld refuses to start a process whose inserted library is missing.
    if(EXISTS /usr/lib/libgmalloc.dylib)
        set_property(TEST test_file_tree APPEND PROPERTY ENVIRONMENT
            "DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib")
    endif()
elseif(UNIX)
    set_property(TEST test_file_tree APPEND PROPERTY ENVIRONMENT "MALLOC_PERTURB_=85")
endif()
# The Rebase dialog, its plan list and the repository view's in-progress bar
gitbolt_add_test(test_rebase_widgets widgets/TestRebaseWidgets.cpp)
target_link_libraries(test_rebase_widgets PRIVATE gitbolt_dialogs)
# It opens the dialog, its target list and the reword prompt: off screen,
# so a local run doesn't flash windows or steal focus (CI runs every test
# off screen anyway).
set_property(TEST test_rebase_widgets APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# dialogs/ — Settings › General › Usage statistics and the
# SettingsService keys behind it. Off screen like test_rebase_widgets:
# no flashing windows, and its Copy test needs the offscreen platform's
# in-process clipboard (it skips anywhere else, rather than overwrite
# the user's).
gitbolt_add_test(test_usage_statistics_settings dialogs/TestUsageStatisticsSettings.cpp)
target_link_libraries(test_usage_statistics_settings PRIVATE gitbolt_dialogs gitbolt_telemetry)
set_property(TEST test_usage_statistics_settings APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# The Commit dialog against a real repository: Amend held back while an
# operation waits for its commit, a hook's refusal shown. Off screen.
gitbolt_add_test(test_commit_dialog dialogs/TestCommitDialog.cpp)
target_link_libraries(test_commit_dialog PRIVATE gitbolt_dialogs)
set_property(TEST test_commit_dialog APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ui/ — the Settings switch reaching usage statistics through a real
# MainWindow's own SettingsService, against the loopback HttpStub.
gitbolt_add_test(test_telemetry_switch ui/TestTelemetrySwitch.cpp)
target_sources(test_telemetry_switch PRIVATE telemetry/HttpStub.h)
target_link_libraries(test_telemetry_switch PRIVATE gitbolt_ui gitbolt_telemetry Qt6::Network)
set_property(TEST test_telemetry_switch APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# File > Recent Repositories following the recent list, and the
# dashboard's list keeping each repository's probe on its own row.
gitbolt_add_test(test_recent_menu ui/TestRecentMenu.cpp)
target_link_libraries(test_recent_menu PRIVATE gitbolt_ui)
set_property(TEST test_recent_menu APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
gitbolt_add_test(test_dashboard_recent ui/TestDashboardRecent.cpp)
target_link_libraries(test_dashboard_recent PRIVATE gitbolt_ui)
set_property(TEST test_dashboard_recent APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
