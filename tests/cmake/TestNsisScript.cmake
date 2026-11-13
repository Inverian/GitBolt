# cmake -P this
# Checks cmake/NsisScript.cmake as NSIS gets it (no box a silent install
# can't answer, files in use not skipped), and that this CMake's
# NSIS.template.in still has the hooks it relies on.

set(PROJECT_VERSION 0.1.0)
set(GITBOLT_WINDOWS_MINIMUM 10.0.19045)
set(CPACK_NSIS_DISPLAY_NAME GitBolt)  # as cmake/Packaging.cmake sets it
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/NsisScript.cmake)

set(_failed FALSE)
# `condition`: if()'s arguments, as one string.
function(_check condition message)
    cmake_language(EVAL CODE "if(${condition})\nset(_ok TRUE)\nelse()\nset(_ok FALSE)\nendif()")
    if(NOT _ok)
        message(SEND_ERROR "${message}")
        set(_failed TRUE PARENT_SCOPE)
    endif()
endfunction()
# Where `text` first holds `part` (-1: nowhere).
function(_find text part out)
    string(FIND "${text}" "${part}" _at)
    set(${out} ${_at} PARENT_SCOPE)
endfunction()

# What NSIS gets: CPack writes each value as set(<name> "<value>") into
# CPackConfig.cmake (no CPACK_VERBATIM_VARIABLES), which cpack reads back.
set(_fragments DEFINES EXTRA_PREINSTALL_COMMANDS EXTRA_INSTALL_COMMANDS CREATE_ICONS_EXTRA
               DELETE_ICONS_EXTRA EXTRA_UNINSTALL_COMMANDS)
foreach(_name IN LISTS _fragments)
    cmake_language(EVAL CODE "set(_nsis_${_name} \"${CPACK_NSIS_${_name}}\")")
endforeach()

_check("CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL STREQUAL \"OFF\""
       "CPack's own uninstall-before-install is on: its 'Uninstall failed.' box hangs a silent install")
_check("CPACK_PACKAGE_INSTALL_REGISTRY_KEY STREQUAL \"GitBolt\" AND CPACK_NSIS_UNINSTALL_NAME STREQUAL \"Uninstall\""
       "the uninstall key or Uninstall.exe is renamed: the updater and CI look for them")
set(_key "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GitBolt")

# The same key as the updater reads (src/update/Package.h, in C++'s \\).
file(READ ${CMAKE_CURRENT_LIST_DIR}/../../src/update/Package.h _package)
_find("${_package}" "\"SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\GitBolt\"" _at)
_check("_at GREATER -1" "src/update/Package.h doesn't read the key the installer writes, ${_key}")

foreach(_line "AllowSkipFiles off" "!include FileFunc.nsh" "VIProductVersion '0.1.0.0'"
              "VIAddVersionKey 'GitBoltMinimumWindows' '10.0.19045'")
    _find("${_nsis_DEFINES}" "\n  ${_line}\n" _at)
    _check("_at GREATER -1" "CPACK_NSIS_DEFINES lacks '${_line}'")
endforeach()

# The registered uninstaller, only if it is there, run from a copy, and
# installed over if it fails; never a box. Then the new one registered
# before any file, so an install that stops halfway can be repaired.
set(_order
    "ReadRegStr $0 HKLM '${_key}' 'UninstallString'"
    "StrCmp $1 '\"' 0 +2"
    "StrCpy $1 $0 '' -14"
    "StrCmp $1 '\\Uninstall.exe' 0 gitbolt_uninstalled"
    "IfFileExists $0 0 gitbolt_uninstalled"
    "CopyFiles /SILENT $0 '$PLUGINSDIR\\gitbolt-old-uninstaller.exe'"
    "ExecWait '\"$PLUGINSDIR\\gitbolt-old-uninstaller.exe\" /S _?=$1' $2"
    "StrCmp $2 0 gitbolt_uninstalled"
    "gitbolt_uninstall_failed:"
    "gitbolt_uninstalled:"
    "SetOutPath $INSTDIR"
    "WriteUninstaller '$INSTDIR\\Uninstall.exe'"
    "WriteRegStr SHCTX '${_key}' 'DisplayName' 'GitBolt'"
    "WriteRegStr SHCTX '${_key}' 'UninstallString' '\"$INSTDIR\\Uninstall.exe\"'")
set(_last -1)
foreach(_line IN LISTS _order)
    _find("${_nsis_EXTRA_PREINSTALL_COMMANDS}" "${_line}\n" _at)
    _check("_at GREATER _last" "CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS lacks '${_line}', or has it out of order")
    set(_last ${_at})
endforeach()

# The uninstaller written again if it's missing; what Installed apps shows.
foreach(_line
        "WriteRegStr HKCR 'Directory\\shell\\gitbolt\\command' '' '\"$INSTDIR\\bin\\GitBolt.exe\" \"%V\"'"
        "IfFileExists '$INSTDIR\\Uninstall.exe' gitbolt_have_uninstaller"
        "WriteUninstaller '$INSTDIR\\Uninstall.exe'"
        "Abort 'Could not create the uninstaller. Run the installer again.'"
        "WriteRegStr SHCTX '${_key}' 'InstallLocation' '$INSTDIR'"
        "\${GetSize} '$INSTDIR' '/S=0K' $0 $1 $2"
        "WriteRegDWORD SHCTX '${_key}' 'EstimatedSize' $0")
    _find("${_nsis_EXTRA_INSTALL_COMMANDS}" "${_line}\n" _at)
    _check("_at GREATER -1" "CPACK_NSIS_EXTRA_INSTALL_COMMANDS lacks '${_line}'")
endforeach()

# Every fragment, line by line: whole quotes (none holds an apostrophe),
# and a MessageBox has /SD or only shows when not silent.
foreach(_name IN LISTS _fragments)
    string(REPLACE ";" "\\;" _text "${_nsis_${_name}}")
    string(REPLACE "\n" ";" _lines "${_text}")
    set(_silent_checked FALSE)
    foreach(_line IN LISTS _lines)
        string(REGEX REPLACE "[^']" "" _quotes "${_line}")
        string(LENGTH "${_quotes}" _count)
        math(EXPR _odd "${_count} % 2")
        _check("_odd EQUAL 0" "${_name}: unbalanced quotes in '${_line}'")
        if(_line MATCHES "^[ \t]*IfSilent ")
            set(_silent_checked TRUE)
        elseif(_line MATCHES "MessageBox" AND NOT _line MATCHES " /SD ID" AND NOT _silent_checked)
            _check("FALSE" "${_name}: a MessageBox a silent install can't answer: '${_line}'")
        endif()
    endforeach()
endforeach()

# Labels: defined once in their Section (the installing one, or the
# uninstaller's), and every one used is defined there.
foreach(_section "EXTRA_PREINSTALL_COMMANDS;EXTRA_INSTALL_COMMANDS"
                 "EXTRA_UNINSTALL_COMMANDS;DELETE_ICONS_EXTRA")
    set(_text "")
    foreach(_name IN LISTS _section)
        string(APPEND _text "${_nsis_${_name}}\n")
    endforeach()
    string(REGEX MATCHALL "\n[ \t]*gitbolt_[a-z_]+:" _defined "${_text}")
    string(REGEX REPLACE "[\n \t:]" "" _defined "${_defined}")
    string(REGEX MATCHALL "gitbolt_[a-z_]+" _used "${_text}")
    foreach(_label IN LISTS _used)
        list(FIND _defined ${_label} _at)
        _check("_at GREATER -1" "${_section}: '${_label}' is used but not defined")
    endforeach()
    list(LENGTH _defined _count)
    list(REMOVE_DUPLICATES _defined)
    list(LENGTH _defined _unique)
    _check("_count EQUAL _unique" "${_section}: a label is defined twice")
endforeach()

# This CMake's template: where each hook runs.
set(_template "")
foreach(_candidate ${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in ${CMAKE_ROOT}/Modules/NSIS.template.in)
    if(EXISTS ${_candidate})
        set(_template ${_candidate})
        break()
    endif()
endforeach()
_check("_template" "no NSIS.template.in in ${CMAKE_ROOT}/Modules")
if(_template)
    file(READ ${_template} _nsi)
    string(REPLACE "\r\n" "\n" _nsi "${_nsi}")
    # In this order: the defines before any Section; in the installing
    # Section, the preinstall commands just before the files, then the
    # uninstaller and its key, then the extra commands; .onInit's
    # uninstall only when switched on; in the uninstaller's Section, its
    # extra commands before anything is deleted.
    set(_hooks
        "@CPACK_NSIS_DEFINES@"
        "\nSection \"-Core installation\""
        "SetOutPath \"$INSTDIR\"\n  @CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS@\n  @CPACK_NSIS_FULL_INSTALL@"
        "WriteUninstaller \"$INSTDIR\\@CPACK_NSIS_UNINSTALL_NAME@.exe\""
        "Push \"UninstallString\"\n  Push \"$\\\"$INSTDIR\\@CPACK_NSIS_UNINSTALL_NAME@.exe$\\\"\""
        "\n@CPACK_NSIS_EXTRA_INSTALL_COMMANDS@\n"
        "\nSectionEnd"
        "\nSection \"Uninstall\""
        "\n@CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS@\n"
        "\n@CPACK_NSIS_DELETE_FILES@\n"
        "\n@CPACK_NSIS_DELETE_ICONS_EXTRA@\n"
        "\nFunction .onInit\n  StrCmp \"@CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL@\" \"ON\" 0 inst\n")
    set(_last -1)
    foreach(_hook IN LISTS _hooks)
        _find("${_nsi}" "${_hook}" _at)
        _check("_at GREATER _last" "${_template} lacks '${_hook}', or has it out of order")
        if(_at GREATER -1)
            set(_last ${_at})
        endif()
    endforeach()
    _find("${_nsi}" "\nSection " _first)
    _find("${_nsi}" "@CPACK_NSIS_DEFINES@" _at)
    _check("_at LESS _first" "${_template}: CPACK_NSIS_DEFINES no longer comes before the first Section")
    # The key the uninstall entry is written to, by SHCTX: HKLM, as the
    # installer runs as an administrator.
    _find("${_nsi}" "WriteRegStr SHCTX \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@\"" _at)
    _check("_at GREATER -1" "${_template} writes the uninstall entry somewhere else")
endif()

if(NOT _failed)
    message(STATUS "NSIS script: as expected, and ${_template} has its hooks")
endif()
