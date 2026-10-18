# GitBolt test suite
#
# Each test file has its own QTEST_MAIN and compiles to its own executable.
# CTest then runs them all as separate tests so one failure doesn't block
# the rest.
#
# Helper function to reduce boilerplate: gitbolt_add_test(<name> <source>)
# creates an executable, links the libraries it needs, and registers it
# with CTest.

# GITBOLT_QTEST_ARGS: QtTest options for every binary ctest runs. The
# Windows CI passes -nocrashhandler: QtTest's own handler ends the
# process itself (its filter returns EXCEPTION_EXECUTE_HANDLER), so
# Windows Error Reporting never sees the crash and writes no dump, and
# its stack walk stops dead at a call through a null pointer (CI saw
# exactly that: "Exception address: 0x0000000000000000" and no frames).
# Without it, WER's LocalDumps (set up by the CI step) keep a minidump,
# whose stack memory still holds the caller's return address. ctest
# reports the crash the same way ("Exception: SegFault"), and QtTest
# still flushes each PASS line, so the last one names the function
# before the one that crashed.
set(GITBOLT_QTEST_ARGS "" CACHE STRING
    "Extra QtTest command-line options for every unit test binary ctest runs (e.g. -nocrashhandler)")
separate_arguments(GITBOLT_QTEST_ARGV NATIVE_COMMAND "${GITBOLT_QTEST_ARGS}")

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
    add_test(NAME ${test_name} COMMAND ${test_name} ${GITBOLT_QTEST_ARGV})
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
    # teardown. No function comes close to 60 s; most binaries take
    # seconds, and the slow ones get a longer TIMEOUT of their own.
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
# GitProcess::run(): a timeout stops git's whole process group (a job
# object on Windows), gently first, so git rolls back its index.lock
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
# The bundled libgit2's patch script (cmake/PatchLibgit2.cmake) on a
# stand-in tree: a fresh one, one it patched, one an earlier version of
# it patched, one patched in a way it doesn't know. Needs no libgit2.
add_test(NAME test_patch_libgit2
    COMMAND ${CMAKE_COMMAND}
        -DPATCH_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/PatchLibgit2.cmake
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/patch-libgit2
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestPatchLibgit2.cmake)

# util/ tests — AsyncRunner's threading guarantees (no repository needed)
gitbolt_add_test(test_async_runner util/TestAsyncRunner.cpp)
# The login shell's PATH for git's children (util/LoginPath.h): a
# Dock-launched GitBolt had launchd's, and a hook that needs node failed.
gitbolt_add_test(test_login_path util/TestLoginPath.cpp)
# English plural forms (util::installEnglishPlurals): "1 file", "2
# files", and both forms of every %n text in the .ts file, as the .qm
# built into gitbolt_util gives them.
gitbolt_add_test(test_english_plurals util/TestEnglishPlurals.cpp)
target_compile_definitions(test_english_plurals PRIVATE
    GITBOLT_PLURALS_TS="${CMAKE_SOURCE_DIR}/translations/gitbolt_en.ts")

# telemetry/ — the client against a loopback HTTP stub (HttpStub.h).
# Never a real endpoint: test binaries don't link src/app, where the
# endpoints live.
gitbolt_add_test(test_telemetry telemetry/TestTelemetry.cpp)
target_sources(test_telemetry PRIVATE telemetry/HttpStub.h telemetry/CrashHeader.h)
target_link_libraries(test_telemetry PRIVATE gitbolt_telemetry Qt6::Network)
# Crash reports: fixtures for every refusal, the renderHeader round
# trip, and (POSIX) death tests that parse the crash.log a real
# handler wrote. Their helper is built as "gitbolt", in a directory of
# its own, so the parser counts its frames as the app's.
gitbolt_add_test(test_crash_log_parser telemetry/TestCrashLogParser.cpp)
target_sources(test_crash_log_parser PRIVATE telemetry/CrashHeader.h)
target_link_libraries(test_crash_log_parser PRIVATE gitbolt_telemetry)
target_compile_definitions(test_crash_log_parser PRIVATE
    GITBOLT_CRASH_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/crash")
if(NOT WIN32)
    add_executable(gitbolt_crash_helper telemetry/CrashHelper.cpp)
    # dladdr, as for gitbolt_util: in libdl before glibc 2.34
    target_link_libraries(gitbolt_crash_helper PRIVATE gitbolt_util ${CMAKE_DL_LIBS})
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
# The log's and the blame view's relative dates: "1 day ago", "2 days ago"
gitbolt_add_test(test_relative_dates   models/TestRelativeDates.cpp)

# services/ tests — GitService orchestration against real temp repos.
# Needs the services lib on top of the defaults the helper links, and
# StallTransport for cancelling a fetch that never ends.
gitbolt_add_test(test_git_service services/TestGitService.cpp)
target_link_libraries(test_git_service PRIVATE gitbolt_services)
add_dependencies(test_git_service gitbolt_stall_transport)
target_compile_definitions(test_git_service PRIVATE
    GITBOLT_TEST_STALL_TRANSPORT="$<TARGET_FILE:gitbolt_stall_transport>")
# Over a hundred functions against real repositories and git: about
# 40 s here, but up to 146 s on windows-latest, too close to 180 s.
set_tests_properties(test_git_service PROPERTIES TIMEOUT 420)

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
# The diff viewer's colours, and the other views' coloured text, in the
# light and dark themes (ThemeService switches them, as in the app). Off
# screen: it shows and grabs widgets.
gitbolt_add_test(test_diff_colors widgets/TestDiffColors.cpp)
target_link_libraries(test_diff_colors PRIVATE gitbolt_dialogs gitbolt_conf gitbolt_ui)
set_property(TEST test_diff_colors APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# The conflict resolver's panes: a conflict on a symbolic link says so,
# and is resolved by accepting a side. Off screen.
gitbolt_add_test(test_merge_conflict_widget widgets/TestMergeConflictWidget.cpp)
target_link_libraries(test_merge_conflict_widget PRIVATE gitbolt_widgets)
set_property(TEST test_merge_conflict_widget APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

# Help › About's minimum size versus its wrapped text, on a short
# off-screen screen (fixtures/short-screen.json). The config file is
# named relative to the working directory: Qt splits the platform spec
# at every ':', so an absolute Windows path (C:/...) would break it.
# With the app's .qrc, for the dialog's real icon.
gitbolt_add_test(test_about_dialog dialogs/TestAboutDialog.cpp)
target_sources(test_about_dialog PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app/resources/gitbolt.qrc)
target_link_libraries(test_about_dialog PRIVATE gitbolt_dialogs)
set_tests_properties(test_about_dialog PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_property(TEST test_about_dialog APPEND PROPERTY ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen:configfile=short-screen.json")

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
# Repository > Edit .gitignore, .gitattributes and .mailmap: a symbolic
# link there (one a repository committed) is neither read nor written
# through. Off screen; it answers the editor and the message boxes.
gitbolt_add_test(test_edit_config_file ui/TestEditConfigFile.cpp)
target_link_libraries(test_edit_config_file PRIVATE gitbolt_ui)
set_property(TEST test_edit_config_file APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# A real MainWindow while git works on its repository: the actions,
# the toolbar's branch switch and the sidebar off during a commit's
# hooks; Checkout as local branch and a submodule's Update as local ops;
# what Abort and Skip say where git keeps the commits; a local op's word
# back on screen after a remote op that overlapped it.
gitbolt_add_test(test_repo_action_locks ui/TestRepoActionLocks.cpp)
target_sources(test_repo_action_locks PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app/resources/gitbolt.qrc)
target_link_libraries(test_repo_action_locks PRIVATE gitbolt_ui)
set_property(TEST test_repo_action_locks APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# A real window per test, and sixteen hand-off flows each given a second
# to do what they mustn't: about a minute here, so more room than the
# 180 s the others get, for a slow runner.
set_tests_properties(test_repo_action_locks PROPERTIES TIMEOUT 420)
# File History: one load per file at a time, with a busy cursor and a
# status-bar note until its dialog comes up.
gitbolt_add_test(test_file_history ui/TestFileHistory.cpp)
target_link_libraries(test_file_history PRIVATE gitbolt_ui)
set_property(TEST test_file_history APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# app/ tests — CHANGELOG.md as Help → Changelog shows it. Built with the
# app's own .qrc so it reads the embedded text, not just the source file.
gitbolt_add_test(test_changelog app/TestChangelog.cpp)
target_sources(test_changelog PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app/resources/gitbolt.qrc)
target_compile_definitions(test_changelog PRIVATE
    GITBOLT_VERSION="${PROJECT_VERSION}"
    GITBOLT_CHANGELOG_PATH="${PROJECT_SOURCE_DIR}/CHANGELOG.md")
# The single-instance guard: lock, hand-off and listener, on every
# platform. The e2e tests that launch the app twice are Unix-only, and
# only this reaches Windows' named pipes. InstanceHelper is either end
# of a hand-off in a process of its own, with no window or settings.
gitbolt_add_test(test_single_instance app/TestSingleInstance.cpp)
target_link_libraries(test_single_instance PRIVATE gitbolt_instance)
add_executable(gitbolt_instance_helper app/InstanceHelper.cpp)
target_link_libraries(gitbolt_instance_helper PRIVATE gitbolt_instance)
gitbolt_set_warnings(gitbolt_instance_helper)
add_dependencies(test_single_instance gitbolt_instance_helper)
target_compile_definitions(test_single_instance PRIVATE
    GITBOLT_INSTANCE_HELPER="$<TARGET_FILE:gitbolt_instance_helper>")

# File › Recent and the home screen's list: one entry per repository,
# whatever spelling of its folder it was opened by.
gitbolt_add_test(test_recent_repositories conf/TestRecentRepositories.cpp)
target_link_libraries(test_recent_repositories PRIVATE gitbolt_conf)

# The folders macOS asks a running GitBolt to open (Dock drops, `open -a`,
# Finder's service): held, coalesced and emitted queued, on every
# platform; the events are sent as Qt sends them.
gitbolt_add_test(test_open_requests app/TestOpenRequests.cpp)
target_link_libraries(test_open_requests PRIVATE gitbolt_instance)

if(APPLE)
    # Finder's "Open in GitBolt" provider, called with the pasteboards
    # Finder sends (nothing is registered with macOS), and the
    # Info.plist keys it and the folder open events need.
    gitbolt_add_test(test_mac_services app/TestMacServices.cpp)
    target_sources(test_mac_services PRIVATE
        app/MacServicesTestSupport.mm app/MacServicesTestSupport.h)
    set_source_files_properties(app/MacServicesTestSupport.mm PROPERTIES
        COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(test_mac_services PRIVATE gitbolt_macservices)
    add_test(NAME macos_info_plist
        COMMAND ${CMAKE_COMMAND}
                -DPLIST=$<TARGET_BUNDLE_CONTENT_DIR:gitbolt>/Info.plist
                -P ${CMAKE_CURRENT_SOURCE_DIR}/app/CheckMacInfoPlist.cmake)
endif()

# The Nautilus extension offers Open in GitBolt where GitBolt finds a
# repository (Repository::openContaining). Plain Python with
# gi.repository stubbed, so Nautilus need not be installed.
if(NOT WIN32)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME nautilus_extension
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/app/test_nautilus_extension.py
                    ${PROJECT_SOURCE_DIR}/src/app/platform/linux/nautilus-gitbolt.py)
    endif()
endif()

# e2e/ tests — the real app binary driven through the test bridge.
add_subdirectory(e2e)
