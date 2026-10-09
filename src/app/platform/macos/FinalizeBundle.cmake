# Install-time finishing of GitBolt.app, run after the Qt deploy script
# (macdeployqt) by install(SCRIPT) in src/app/CMakeLists.txt. It also
# runs inside CPack's staging install, so it is what the DMG ships.
# Expects GITBOLT_APP (the installed bundle) and GITBOLT_MIN_MACOS
# (CMAKE_OSX_DEPLOYMENT_TARGET) from the install(CODE) just before it.
#
# 1. Deletes every LC_RPATH that points outside the bundle from every
#    Mach-O in it. Homebrew-built dylibs that macdeployqt copies in keep
#    rpaths into /opt/homebrew/Cellar, and Homebrew Qt's plugins carry
#    one that climbs out of the .app; on a Mac with Homebrew, dyld would
#    search those and could load Homebrew's libraries instead of ours.
# 2. Resolves every load command the way dyld would, within the bundle:
#    @loader_path/@executable_path relative to the image or GitBolt,
#    @rpath/<x> in the image's remaining LC_RPATHs, then GitBolt's own
#    (@executable_path/../Frameworks; dyld also tries those of the
#    images in between, so this is if anything stricter). Fails on a
#    library outside the bundle and the OS (/System, /usr/lib): that app
#    would only run here. Fails on one that should be inside but isn't
#    there, except below Contents/PlugIns, which only warns: Homebrew's
#    Qt has plugins (imageformats/libqpdf, the virtual keyboard) whose
#    frameworks macdeployqt doesn't deploy; they can never load, but the
#    app runs without them. CI's DMG smoke test fails on those as well.
#    Weak imports may be missing, as at run time.
# 3. Warns when images need a newer macOS than the deployment target,
#    i.e. than LSMinimumSystemVersion (Homebrew's Qt; CI's aqt Qt fits).
# 4. Re-signs ad hoc and verifies. macdeployqt strips every framework
#    and dylib it copies and rewrites their install names, which
#    invalidates their signatures (as does step 1), and Qt 6.10's
#    macdeployqt does not re-sign (ad-hoc signing became its default
#    only in 6.11, QTBUG-138019). An invalid signature is what
#    Gatekeeper shows as "GitBolt is damaged".

# cmake_install.cmake sets no policy version. 3.21 is the project's:
# IN_LIST (CMP0057), and GLOB_RECURSE not following symlinks (CMP0009),
# which would list each framework binary twice via Versions/Current.
cmake_policy(PUSH)
cmake_policy(VERSION 3.21)

# <ref> with @loader_path (the directory of <file>) and @executable_path
# (GitBolt's Contents/MacOS) expanded; anything else (an absolute path)
# is taken as is. Sets <out_path> to the normalized result and
# <out_inside> to whether it lies within GITBOLT_APP.
function(_gitbolt_expand ref file out_path out_inside)
    if(ref MATCHES "^@loader_path(/.*)?$")
        get_filename_component(_dir "${file}" DIRECTORY)
        set(_path "${_dir}${CMAKE_MATCH_1}")
    elseif(ref MATCHES "^@executable_path(/.*)?$")
        set(_path "${GITBOLT_APP}/Contents/MacOS${CMAKE_MATCH_1}")
    else()
        set(_path "${ref}")
    endif()
    cmake_path(NORMAL_PATH _path)
    string(FIND "${_path}/" "${GITBOLT_APP}/" _at)
    set(${out_path} "${_path}" PARENT_SCOPE)
    if(_at EQUAL 0)
        set(${out_inside} TRUE PARENT_SCOPE)
    else()
        set(${out_inside} FALSE PARENT_SCOPE)
    endif()
endfunction()

# LC_RPATH paths, dylib load names (weak ones separately) and the
# highest minos of <file>. otool prints each command's fields on their
# own lines, and a universal binary's once per architecture.
function(_gitbolt_load_commands file rpaths_out loads_out weak_out minos_out)
    execute_process(COMMAND otool -l "${file}"
        OUTPUT_VARIABLE _lc RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "otool -l ${file} failed: ${_err}")
    endif()
    string(REGEX MATCHALL "cmd LC_RPATH\n[^\n]*\n *path [^\n]* \\(offset [0-9]+\\)"
           _rp "${_lc}")
    list(TRANSFORM _rp REPLACE ".*\n *path (.*) \\(offset [0-9]+\\)$" "\\1")
    string(REGEX MATCHALL
           "cmd LC_(LOAD|REEXPORT|LAZY_LOAD|LOAD_UPWARD)_DYLIB\n[^\n]*\n *name [^\n]* \\(offset [0-9]+\\)"
           _ld "${_lc}")
    list(TRANSFORM _ld REPLACE ".*\n *name (.*) \\(offset [0-9]+\\)$" "\\1")
    string(REGEX MATCHALL
           "cmd LC_LOAD_WEAK_DYLIB\n[^\n]*\n *name [^\n]* \\(offset [0-9]+\\)"
           _wk "${_lc}")
    list(TRANSFORM _wk REPLACE ".*\n *name (.*) \\(offset [0-9]+\\)$" "\\1")
    string(REGEX MATCHALL "cmd LC_BUILD_VERSION\n[^\n]*\n[^\n]*\n *minos [0-9.]+" _mv "${_lc}")
    list(TRANSFORM _mv REPLACE ".*minos " "")
    set(_minos "")
    if(_mv)
        list(SORT _mv COMPARE NATURAL ORDER DESCENDING)
        list(GET _mv 0 _minos)
    endif()
    list(REMOVE_DUPLICATES _rp)
    list(REMOVE_DUPLICATES _ld)
    list(REMOVE_DUPLICATES _wk)
    set(${rpaths_out} "${_rp}" PARENT_SCOPE)
    set(${loads_out} "${_ld}" PARENT_SCOPE)
    set(${weak_out} "${_wk}" PARENT_SCOPE)
    set(${minos_out} "${_minos}" PARENT_SCOPE)
endfunction()

function(_gitbolt_finalize_bundle)
    cmake_path(NORMAL_PATH GITBOLT_APP)
    string(REGEX REPLACE "/$" "" GITBOLT_APP "${GITBOLT_APP}")
    set(_exe "${GITBOLT_APP}/Contents/MacOS/GitBolt")
    if(NOT EXISTS "${_exe}")
        message(FATAL_ERROR "${_exe} not found")
    endif()
    file(GLOB_RECURSE _files LIST_DIRECTORIES false "${GITBOLT_APP}/Contents/*")

    # Step 1
    set(_macho "")
    set(_stripped "")
    foreach(_f IN LISTS _files)
        if(IS_SYMLINK "${_f}")
            continue()
        endif()
        # Mach-O 64-bit, 32-bit or universal
        file(READ "${_f}" _magic LIMIT 4 HEX)
        if(NOT _magic MATCHES "^(cffaedfe|cefaedfe|cafebabe)$")
            continue()
        endif()
        list(APPEND _macho "${_f}")
        file(RELATIVE_PATH _rel "${GITBOLT_APP}" "${_f}")

        # A path can appear more than once; go until none is left.
        foreach(_round RANGE 5)
            _gitbolt_load_commands("${_f}" _rpaths _loads _weak _minos)
            set(_delete "")
            foreach(_rp IN LISTS _rpaths)
                _gitbolt_expand("${_rp}" "${_f}" _path _in)
                if(NOT _in)
                    list(APPEND _delete -delete_rpath "${_rp}")
                    list(APPEND _stripped "${_rel}: ${_rp}")
                endif()
            endforeach()
            if(NOT _delete)
                break()
            endif()
            # Its "will invalidate the code signature" warning is
            # expected (step 4); show the output only on failure.
            execute_process(COMMAND install_name_tool ${_delete} "${_f}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "install_name_tool ${_delete} ${_rel} failed:\n${_out}")
            endif()
        endforeach()
    endforeach()
    list(LENGTH _macho _count)
    if(_count EQUAL 0)
        message(FATAL_ERROR "no Mach-O files found in ${GITBOLT_APP}")
    endif()
    list(REMOVE_DUPLICATES _stripped)
    list(LENGTH _stripped _n)
    message(STATUS "GitBolt.app: ${_count} Mach-O files, ${_n} LC_RPATH(s) outside the bundle deleted")
    foreach(_s IN LISTS _stripped)
        message(STATUS "  ${_s}")
    endforeach()

    # Steps 2 and 3. GitBolt's LC_RPATHs, expanded relative to GitBolt,
    # end every image's @rpath search.
    _gitbolt_load_commands("${_exe}" _rpaths _loads _weak _minos)
    set(_exe_dirs "")
    foreach(_rp IN LISTS _rpaths)
        _gitbolt_expand("${_rp}" "${_exe}" _path _in)
        list(APPEND _exe_dirs "${_path}")
    endforeach()

    set(_foreign "")
    set(_missing "")
    set(_missing_plugins "")
    set(_newer "")
    foreach(_f IN LISTS _macho)
        file(RELATIVE_PATH _rel "${GITBOLT_APP}" "${_f}")
        _gitbolt_load_commands("${_f}" _rpaths _loads _weak _minos)
        set(_dirs "")
        foreach(_rp IN LISTS _rpaths)
            _gitbolt_expand("${_rp}" "${_f}" _path _in)
            list(APPEND _dirs "${_path}")
        endforeach()
        list(APPEND _dirs ${_exe_dirs})

        foreach(_kind IN ITEMS _loads _weak)
            foreach(_ref IN LISTS ${_kind})
                if(_ref MATCHES "^(/System/|/usr/lib/)")
                    continue()   # the OS's, in the dyld shared cache
                endif()
                set(_found FALSE)
                if(_ref MATCHES "^@rpath(/.+)$")
                    set(_tail "${CMAKE_MATCH_1}")
                    foreach(_dir IN LISTS _dirs)
                        _gitbolt_expand("${_dir}${_tail}" "${_f}" _path _in)
                        if(_in AND EXISTS "${_path}")
                            set(_found TRUE)
                            break()
                        endif()
                    endforeach()
                else()
                    _gitbolt_expand("${_ref}" "${_f}" _path _in)
                    if(NOT _in)
                        list(APPEND _foreign "${_rel} -> ${_ref}")
                        continue()
                    endif()
                    if(EXISTS "${_path}")
                        set(_found TRUE)
                    endif()
                endif()
                if(_found OR _kind STREQUAL "_weak")
                    continue()
                elseif(_rel MATCHES "^Contents/PlugIns/")
                    list(APPEND _missing_plugins "${_rel} -> ${_ref}")
                else()
                    list(APPEND _missing "${_rel} -> ${_ref}")
                endif()
            endforeach()
        endforeach()

        if(_minos AND GITBOLT_MIN_MACOS AND _minos VERSION_GREATER GITBOLT_MIN_MACOS)
            list(APPEND _newer "${_minos} ${_rel}")
        endif()
    endforeach()

    if(_foreign)
        list(JOIN _foreign "\n  " _list)
        message(FATAL_ERROR "GitBolt.app refers to libraries outside the bundle and the OS, "
                            "so it would only run on this machine:\n  ${_list}")
    endif()
    if(_missing)
        list(JOIN _missing "\n  " _list)
        message(FATAL_ERROR "GitBolt.app needs libraries that are not in the bundle:\n  ${_list}")
    endif()
    if(_missing_plugins)
        list(JOIN _missing_plugins "\n  " _list)
        message(WARNING "These Qt plugins in GitBolt.app need libraries that are not in "
                        "the bundle, so they can never load (CI's DMG smoke test rejects "
                        "this):\n  ${_list}")
    endif()
    if(_newer)
        list(SORT _newer COMPARE NATURAL ORDER DESCENDING)
        list(LENGTH _newer _n)
        list(GET _newer 0 _top)
        message(WARNING "${_n} Mach-O files in GitBolt.app need a newer macOS than "
                        "CMAKE_OSX_DEPLOYMENT_TARGET (= LSMinimumSystemVersion) "
                        "${GITBOLT_MIN_MACOS}, so this bundle won't start there "
                        "(highest: minos ${_top}). Build against a Qt whose "
                        "binaries target ${GITBOLT_MIN_MACOS}, as CI does, or "
                        "raise CMAKE_OSX_DEPLOYMENT_TARGET.")
    endif()

    message(STATUS "Ad-hoc signing ${GITBOLT_APP}")
    execute_process(
        COMMAND codesign --force --deep --sign - "${GITBOLT_APP}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND codesign --verify --deep --strict "${GITBOLT_APP}"
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

_gitbolt_finalize_bundle()
cmake_policy(POP)
