# cmake -DWORK_DIR=<empty scratch dir> -P this
#
# The Linux install step that leaves each private library under its
# soname alone (src/app/platform/linux/FlattenLibraryLinks.cmake): what
# Qt's deploy copies, a soname link to the versioned file, becomes one
# file; links it can't flatten safely stay links. CI's Linux leg packages
# the real thing; this runs the rules on every platform.

if(NOT WORK_DIR)
    message(FATAL_ERROR "WORK_DIR not set")
endif()
set(GITBOLT_LIB_DIR "${WORK_DIR}/lib")
file(REMOVE_RECURSE "${GITBOLT_LIB_DIR}")
file(MAKE_DIRECTORY "${GITBOLT_LIB_DIR}" "${WORK_DIR}/elsewhere")

# A soname link to the file beside it, as the deploy leaves Qt's and ICU's.
file(WRITE "${GITBOLT_LIB_DIR}/libQt6Core.so.6.11.3" "core")
file(CREATE_LINK "libQt6Core.so.6.11.3" "${GITBOLT_LIB_DIR}/libQt6Core.so.6" SYMBOLIC)
file(WRITE "${GITBOLT_LIB_DIR}/libicudata.so.73.2" "icu")
file(CREATE_LINK "libicudata.so.73.2" "${GITBOLT_LIB_DIR}/libicudata.so.73" SYMBOLIC)
# A plain file stays as it is.
file(WRITE "${GITBOLT_LIB_DIR}/libplain.so.1" "plain")
# Two links to one file: flattening one would leave the other dangling.
file(WRITE "${GITBOLT_LIB_DIR}/libtwo.so.1.0" "two")
file(CREATE_LINK "libtwo.so.1.0" "${GITBOLT_LIB_DIR}/libtwo.so.1" SYMBOLIC)
file(CREATE_LINK "libtwo.so.1.0" "${GITBOLT_LIB_DIR}/libtwo.so" SYMBOLIC)
# A link out of the folder.
file(WRITE "${WORK_DIR}/elsewhere/libout.so.2.0" "out")
file(CREATE_LINK "../elsewhere/libout.so.2.0" "${GITBOLT_LIB_DIR}/libout.so.2" SYMBOLIC)

include(${CMAKE_CURRENT_LIST_DIR}/../../src/app/platform/linux/FlattenLibraryLinks.cmake)

set(_failed FALSE)
function(expect_file name content)
    set(_path "${GITBOLT_LIB_DIR}/${name}")
    if(IS_SYMLINK "${_path}" OR NOT EXISTS "${_path}")
        message(SEND_ERROR "${name}: should be a file")
        set(_failed TRUE PARENT_SCOPE)
        return()
    endif()
    file(READ "${_path}" _got)
    if(NOT _got STREQUAL content)
        message(SEND_ERROR "${name}: holds '${_got}', should hold '${content}'")
        set(_failed TRUE PARENT_SCOPE)
    endif()
endfunction()
function(expect_link name)
    if(NOT IS_SYMLINK "${GITBOLT_LIB_DIR}/${name}")
        message(SEND_ERROR "${name}: should still be a link")
        set(_failed TRUE PARENT_SCOPE)
    endif()
endfunction()
function(expect_gone name)
    if(EXISTS "${GITBOLT_LIB_DIR}/${name}" OR IS_SYMLINK "${GITBOLT_LIB_DIR}/${name}")
        message(SEND_ERROR "${name}: should be gone")
        set(_failed TRUE PARENT_SCOPE)
    endif()
endfunction()

expect_file(libQt6Core.so.6 "core")
expect_gone(libQt6Core.so.6.11.3)
expect_file(libicudata.so.73 "icu")
expect_gone(libicudata.so.73.2)
expect_file(libplain.so.1 "plain")
expect_link(libtwo.so.1)
expect_link(libtwo.so)
expect_file(libtwo.so.1.0 "two")
expect_link(libout.so.2)
if(NOT _failed)
    message(STATUS "flatten library links: all cases as expected")
endif()
