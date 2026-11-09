# In-app updates (src/update, src/ui/UpdateSetup.h, PRIVACY.md).
#
# GITBOLT_UPDATES: whether this build asks GitHub for a newer GitBolt
# (once a day, and from Help > Check for Updates) and offers to install
# it. Official builds only: CI turns it on for pushes to upstream main,
# with the same condition that fills in the telemetry endpoints
# (.github/workflows/ci.yml, Configure). OFF, the default, is every
# source build, fork, pull request and distribution package: their
# updates are their packager's business, and replacing one with
# Inverian's installer would be wrong. Help > Check for Updates is then
# greyed out (Help > About links the releases page), and Settings has no
# Updates page.
#
# It becomes GITBOLT_UPDATES_ENABLED on the app target only
# (src/app/CMakeLists.txt), which is where the feed's address goes into
# the updater's settings (main.cpp): no library or test binary carries
# it. Test sessions never use it either way; they can point the updater
# at a loopback stub (GITBOLT_TEST_UPDATE_FEED) in any build.
#
# Like the GITBOLT_TELEMETRY_* entries, it takes its first value from the
# environment variable of the same name, so CI passes it as step env. An
# existing build directory keeps its cached value until it is configured
# fresh.

# One of CMake's boolean words, nothing else: option() would read a typo
# such as "Of", or "On " with its space, as OFF without a word.
function(_gitbolt_updates_check value where)
    string(TOUPPER "${value}" _word)
    if(NOT _word MATCHES "^(ON|OFF|TRUE|FALSE|YES|NO|Y|N|1|0)$")
        message(FATAL_ERROR "GITBOLT_UPDATES (${where}) must be ON or OFF, not '${value}'")
    endif()
endfunction()

set(_gitbolt_updates_default OFF)
if(DEFINED ENV{GITBOLT_UPDATES} AND NOT "$ENV{GITBOLT_UPDATES}" STREQUAL "")
    _gitbolt_updates_check("$ENV{GITBOLT_UPDATES}" "environment")
    set(_gitbolt_updates_default "$ENV{GITBOLT_UPDATES}")
endif()
option(GITBOLT_UPDATES
    "Official builds only: check GitHub for new GitBolt releases and install them (ci.yml turns it on for upstream main)"
    ${_gitbolt_updates_default})
# A -DGITBOLT_UPDATES=... value goes into the cache as given.
_gitbolt_updates_check("${GITBOLT_UPDATES}" "cache")

if(GITBOLT_UPDATES)
    message(STATUS "In-app updates: on (official feed, GitHub releases of Inverian/GitBolt)")
else()
    message(STATUS "In-app updates: off (Help > Check for Updates is greyed out)")
endif()
