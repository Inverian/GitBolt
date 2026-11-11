# End-to-end tests: drive the real app binary through the test bridge
# (docs/AGENT_TESTING.md) and assert on-disk git outcomes, on every
# platform. On Windows the harness reaches the bridge as a named pipe,
# and a test, or a part of one, that can't run there skips itself
# (exit 77) and says why.

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    message(STATUS "GitBolt: Python3 not found — e2e tests skipped")
    return()
endif()

set(E2E_TESTS
    test_auto_fetch
    test_auto_fetch_credentials
    test_auto_fetch_disowned
    test_bare_repo
    test_branch_switch
    test_clone_dialog
    test_commit_flow
    test_commit_hooks
    test_crash_box_handoff
    test_desktop_identity
    test_dialog_guards
    test_file_open
    test_git_notice
    test_home_isolated
    test_instance_lock
    test_local_op_async
    test_local_op_guards
    test_quit_during_fetch
    test_quit_during_rebase
    test_rebase_dialog
    test_rebase_progress
    test_remote_branches
    test_remote_op_feedback
    test_remote_op_leave_repo
    test_revert_commit
    test_settings_after_open
    test_single_instance
    test_tags
    test_telemetry_inert
    test_theme_persistence
    test_update_flow
    test_update_inert
    test_watcher_external_commit
)
# Needs macOS's small unix-socket buffers to hold a hand-off line up
# the way Windows' unbuffered pipes hold up every one (see the test).
# Its 64 KB argument is over Windows' 32,767-character command line too.
if(APPLE)
    list(APPEND E2E_TESTS test_instance_busy_owner)
endif()

# Open events through LaunchServices to the real cocoa app. It registers
# the build's GitBolt.app with LaunchServices, so it runs only where CI
# asks for it (GITBOLT_E2E_LAUNCHSERVICES=1 on a throwaway runner) and
# is skipped everywhere else.
if(APPLE)
    list(APPEND E2E_TESTS test_launchservices_open)
endif()

# Windows starts processes, and MSYS sh's hooks, more slowly.
set(E2E_TIMEOUT 120)
if(WIN32)
    set(E2E_TIMEOUT 240)
endif()
foreach(t IN LISTS E2E_TESTS)
    add_test(NAME e2e_${t}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${t}.py
                $<TARGET_FILE:gitbolt>)
    # RUN_SERIAL: each test owns app processes, and watcher/push
    # timing assertions get flaky under parallel CPU contention.
    set_tests_properties(e2e_${t} PROPERTIES
        LABELS "e2e"
        TIMEOUT ${E2E_TIMEOUT}
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
    # UTF-8: Windows' Python would print in the ANSI code page, which
    # lacks some of the app's text a FAIL line quotes (…, ✗).
    if(WIN32)
        set_tests_properties(e2e_${t} PROPERTIES ENVIRONMENT "PYTHONUTF8=1")
    endif()
endforeach()

# A packaged GitBolt (a DMG on macOS, a .deb on Linux) updates in place:
# download, checks, the helper, the relaunch. It needs that package,
# which only CI's packaging jobs (or a local cpack) make, so it runs
# only when GITBOLT_E2E_INSTALL_APP or GITBOLT_E2E_INSTALLED_APP names
# one, and is skipped everywhere else. CI also runs it on Windows,
# directly, against the NSIS install. Its own longer TIMEOUT: hdiutil,
# codesign --deep twice and a copy of the bundle take a minute or two.
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME e2e_test_update_install
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_update_install.py
                $<TARGET_FILE:gitbolt>)
    set_tests_properties(e2e_test_update_install PROPERTIES
        LABELS "e2e"
        TIMEOUT 420
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
endif()
