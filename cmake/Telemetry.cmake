# Usage statistics endpoints (src/telemetry, PRIVACY.md).
#
# Compiled into the app target only (src/app/TelemetryEndpoints.h.in),
# so no library or test binary can carry them. All four, or none. None
# — every source build, fork, pull request and dry-run CI build —
# compiles telemetry out: main() then creates no telemetry service at
# all, and Settings shows no Usage statistics group. CI fills them in
# on pushes to upstream main only (.github/workflows/ci.yml, Configure).
#
#   GITBOLT_TELEMETRY_HOST                 https://<host>[:<port>], no path
#   GITBOLT_TELEMETRY_OPENPANEL_CLIENT_ID  OpenPanel client id
#   GITBOLT_TELEMETRY_OPENPANEL_SECRET     OpenPanel client secret
#   GITBOLT_TELEMETRY_GLITCHTIP_DSN        <scheme>://<32 hex key>@<HOST's host>/<project id>
#
# Plain http only to a loopback stub (http://127.0.0.1:<port> or
# http://[::1]:<port>), for a developer's manual testing; such a build
# is not official (main.cpp), so its crash reports never upload.
#
# Each takes its first value from the environment variable of the same
# name, like CMAKE_OSX_DEPLOYMENT_TARGET from MACOSX_DEPLOYMENT_TARGET:
# CI passes them as step env, never on a shell command line. An
# existing build directory keeps its cached values until it is
# configured fresh, and its plain launches (no test bridge) send: keep
# endpoint builds in a build directory of their own.

set(GITBOLT_TELEMETRY_HOST "$ENV{GITBOLT_TELEMETRY_HOST}" CACHE STRING
    "Telemetry ingest origin, https://<host>; empty compiles telemetry out (all four GITBOLT_TELEMETRY_* or none)")
set(GITBOLT_TELEMETRY_OPENPANEL_CLIENT_ID "$ENV{GITBOLT_TELEMETRY_OPENPANEL_CLIENT_ID}" CACHE STRING
    "OpenPanel client id for usage statistics")
set(GITBOLT_TELEMETRY_OPENPANEL_SECRET "$ENV{GITBOLT_TELEMETRY_OPENPANEL_SECRET}" CACHE STRING
    "OpenPanel client secret for usage statistics")
set(GITBOLT_TELEMETRY_GLITCHTIP_DSN "$ENV{GITBOLT_TELEMETRY_GLITCHTIP_DSN}" CACHE STRING
    "GlitchTip DSN for crash and error reports, https://<key>@<host>/<project>")

# Derived from the DSN for the generated header; empty when compiled out.
set(GITBOLT_TELEMETRY_GLITCHTIP_KEY "")
set(GITBOLT_TELEMETRY_GLITCHTIP_PROJECT "")

set(_gitbolt_telemetry_vars
    GITBOLT_TELEMETRY_HOST
    GITBOLT_TELEMETRY_OPENPANEL_CLIENT_ID
    GITBOLT_TELEMETRY_OPENPANEL_SECRET
    GITBOLT_TELEMETRY_GLITCHTIP_DSN)
set(_gitbolt_telemetry_missing "")
foreach(_var IN LISTS _gitbolt_telemetry_vars)
    if("${${_var}}" STREQUAL "")
        list(APPEND _gitbolt_telemetry_missing ${_var})
    endif()
endforeach()
list(LENGTH _gitbolt_telemetry_missing _gitbolt_telemetry_missing_count)

if(_gitbolt_telemetry_missing_count EQUAL 4)
    message(STATUS "Telemetry: compiled out (no endpoints)")
elseif(_gitbolt_telemetry_missing_count GREATER 0)
    list(JOIN _gitbolt_telemetry_missing ", " _gitbolt_telemetry_missing)
    message(FATAL_ERROR
        "Telemetry: all four GITBOLT_TELEMETRY_* values, or none. Missing: "
        "${_gitbolt_telemetry_missing}")
else()
    # The patterns also keep quotes and backslashes out of the
    # generated C++. CMake regexes have no {n} counts: the key's
    # length is checked on its own.
    set(_gitbolt_loopback_origin "^http://(127\\.0\\.0\\.1|\\[::1\\])(:[0-9]+)?$")
    if(GITBOLT_TELEMETRY_HOST MATCHES "^https://[A-Za-z0-9.-]+(:[0-9]+)?$")
        set(_gitbolt_telemetry_kind "enabled")
    elseif(GITBOLT_TELEMETRY_HOST MATCHES "${_gitbolt_loopback_origin}")
        set(_gitbolt_telemetry_kind "enabled for a local stub")
    else()
        message(FATAL_ERROR
            "Telemetry: GITBOLT_TELEMETRY_HOST must be https://<host>[:<port>] with no "
            "path (plain http only to 127.0.0.1 or [::1]): '${GITBOLT_TELEMETRY_HOST}'")
    endif()

    foreach(_var GITBOLT_TELEMETRY_OPENPANEL_CLIENT_ID GITBOLT_TELEMETRY_OPENPANEL_SECRET)
        if(NOT "${${_var}}" MATCHES "^[A-Za-z0-9_-]+$")
            message(FATAL_ERROR "Telemetry: ${_var} may hold only letters, digits, '_' and '-'")
        endif()
    endforeach()

    if(NOT GITBOLT_TELEMETRY_GLITCHTIP_DSN MATCHES "^(https?://)([0-9a-f]+)@([^/@]+)/([0-9]+)$")
        message(FATAL_ERROR
            "Telemetry: GITBOLT_TELEMETRY_GLITCHTIP_DSN must be <scheme>://<key>@<host>/<project id>")
    endif()
    set(_gitbolt_dsn_origin "${CMAKE_MATCH_1}${CMAKE_MATCH_3}")
    set(GITBOLT_TELEMETRY_GLITCHTIP_KEY "${CMAKE_MATCH_2}")
    set(GITBOLT_TELEMETRY_GLITCHTIP_PROJECT "${CMAKE_MATCH_4}")
    string(LENGTH "${GITBOLT_TELEMETRY_GLITCHTIP_KEY}" _gitbolt_key_length)
    if(NOT _gitbolt_key_length EQUAL 32)
        message(FATAL_ERROR "Telemetry: the GlitchTip DSN's key must be 32 hex digits")
    endif()
    # One origin for both sinks: the ingest proxy routes /track and
    # /api/<id>/store/ (src/telemetry/TelemetryConfig.h).
    if(NOT _gitbolt_dsn_origin STREQUAL GITBOLT_TELEMETRY_HOST)
        message(FATAL_ERROR
            "Telemetry: the GlitchTip DSN must name GITBOLT_TELEMETRY_HOST's origin "
            "(${GITBOLT_TELEMETRY_HOST}), not ${_gitbolt_dsn_origin}")
    endif()

    message(STATUS "Telemetry: ${_gitbolt_telemetry_kind} (${GITBOLT_TELEMETRY_HOST})")
endif()
