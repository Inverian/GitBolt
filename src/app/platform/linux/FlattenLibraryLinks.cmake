# Install-time step for Linux, run after the Qt deploy script by
# install(SCRIPT) in src/app/CMakeLists.txt, so also inside CPack's
# staging install. Expects GITBOLT_LIB_DIR (the private lib dir in the
# tree being installed) from the install(CODE) just before it.
#
# The deploy copies each Qt and ICU library as its soname, a link, and
# the file it names (libQt6Core.so.6 -> libQt6Core.so.6.11.3). The loader
# only ever asks for the soname, and CPack's DEB sums the size of each
# path through its links, so the package's Installed-Size counted every
# library twice. Here the file takes the link's place: one path, the
# soname, for each library. A link to anything else (another folder,
# another link, a file two links name) is left as it is, with a warning.

cmake_policy(PUSH)
cmake_policy(VERSION 3.21)

file(GLOB _gitbolt_paths LIST_DIRECTORIES false "${GITBOLT_LIB_DIR}/*")
set(_gitbolt_links)
set(_gitbolt_targets)
foreach(_path IN LISTS _gitbolt_paths)
    if(IS_SYMLINK "${_path}")
        file(READ_SYMLINK "${_path}" _target)
        list(APPEND _gitbolt_links "${_path}")
        list(APPEND _gitbolt_targets "${_target}")
    endif()
endforeach()

list(LENGTH _gitbolt_links _count)
if(_count GREATER 0)
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE ${_last})
        list(GET _gitbolt_links ${_i} _link)
        list(GET _gitbolt_targets ${_i} _target)
        set(_file "${GITBOLT_LIB_DIR}/${_target}")
        # Named by one link only: a second one would be left dangling.
        set(_names 0)
        foreach(_other IN LISTS _gitbolt_targets)
            if(_other STREQUAL _target)
                math(EXPR _names "${_names} + 1")
            endif()
        endforeach()
        if(_target MATCHES "/" OR IS_SYMLINK "${_file}" OR NOT EXISTS "${_file}"
           OR NOT _names EQUAL 1)
            message(WARNING "${_link} -> ${_target}: not a soname link to a library "
                            "beside it, left as a link")
            continue()
        endif()
        file(REMOVE "${_link}")
        file(RENAME "${_file}" "${_link}")
    endforeach()
endif()

cmake_policy(POP)
