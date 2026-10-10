# End-to-end tests: drive the real app binary through the test bridge
# (docs/AGENT_TESTING.md) and assert on-disk git outcomes.
#
# Unix-only: the harness reaches QLocalServer's named socket as a
# unix-domain socket, which has no Windows equivalent (named pipes).
if(WIN32)
    message(STATUS "GitBolt: e2e tests skipped on Windows (unix sockets)")
    return()
endif()

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    message(STATUS "GitBolt: Python3 not found — e2e tests skipped")
    return()
endif()

set(E2E_TESTS
    test_auto_fetch
    test_auto_fetch_disowned
    test_branch_switch
    test_clone_dialog
    test_commit_flow
    test_instance_lock
    test_quit_during_fetch
    test_rebase_dialog
    test_rebase_progress
    test_remote_branches
    test_remote_op_leave_repo
    test_settings_after_open
    test_single_instance
    test_tags
    test_theme_persistence
    test_watcher_external_commit
)
# Needs macOS's small unix-socket buffers to hold a hand-off line up
# the way Windows' unbuffered pipes hold up every one (see the test).
if(APPLE)
    list(APPEND E2E_TESTS test_instance_busy_owner)
endif()

foreach(t IN LISTS E2E_TESTS)
    add_test(NAME e2e_${t}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${t}.py
                $<TARGET_FILE:gitbolt>)
    # RUN_SERIAL: each test owns app processes, and watcher/push
    # timing assertions get flaky under parallel CPU contention.
    set_tests_properties(e2e_${t} PROPERTIES
        LABELS "e2e"
        TIMEOUT 120
        RUN_SERIAL TRUE)
endforeach()
