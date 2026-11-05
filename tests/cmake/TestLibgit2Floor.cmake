# cmake -P this
#
# The oldest system libgit2 GitBolt takes (cmake/Libgit2Floor.cmake). CI
# builds the bundled one only, so this is where the floor is tried.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/Libgit2Floor.cmake)

set(_failed FALSE)
foreach(_case
        "1.7.2:FALSE" "1.8.5:FALSE" "1.8.6:TRUE" "1.8.7:TRUE" "1.9.0:FALSE"
        "1.9.4:FALSE" "1.9.5:TRUE" "1.9.5-dev:TRUE" "1.9.7:TRUE" "2.0.0:TRUE" ":FALSE")
    string(FIND "${_case}" ":" _at REVERSE)
    string(SUBSTRING "${_case}" 0 ${_at} _version)
    math(EXPR _at "${_at} + 1")
    string(SUBSTRING "${_case}" ${_at} -1 _want)
    gitbolt_libgit2_version_ok("${_version}" _got)
    if(NOT _got STREQUAL _want)
        message(SEND_ERROR "libgit2 '${_version}': taken ${_got}, should be ${_want}")
        set(_failed TRUE)
    endif()
endforeach()
if(NOT _failed)
    message(STATUS "libgit2 floor: all cases as expected")
endif()
