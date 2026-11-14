# cmake -P this
# Checks cmake/NsisScript.cmake as NSIS gets it (no box a silent install
# can't answer, a file in use found before anything changes and never
# skipped), and that this CMake's NSIS.template.in still has the hooks it
# relies on.

# Policies as the project's (CMP0053: @VAR@ in quoted arguments stays text).
cmake_minimum_required(VERSION 3.21)

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

# The in-use exit code, as the updater reads it (src/update/InstallHelpers.h):
# none of NSIS's own (1, 2) nor the administrator part's (kWindowsExit*).
file(READ ${CMAKE_CURRENT_LIST_DIR}/../../src/update/InstallHelpers.h _helpers)
string(REGEX MATCH "kWindowsInstallerExitInUse = ([0-9]+);" _match "${_helpers}")
set(_in_use_exit "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "kWindowsExit[A-Za-z]+ = [0-9]+;" _admin_exits "${_helpers}")
string(REGEX REPLACE "kWindowsExit[A-Za-z]+ = ([0-9]+);" "\\1" _admin_exits "${_admin_exits}")
if(_in_use_exit STREQUAL "")
    _check("FALSE" "src/update/InstallHelpers.h has no kWindowsInstallerExitInUse = <code>;")
else()
    list(FIND _admin_exits ${_in_use_exit} _at)
    _check("_in_use_exit GREATER 2 AND _at EQUAL -1"
           "kWindowsInstallerExitInUse (${_in_use_exit}) is NSIS's own exit code or the administrator part's")
endif()

foreach(_line "AllowSkipFiles off" "!include FileFunc.nsh" "VIProductVersion '0.1.0.0'"
              "VIAddVersionKey 'GitBoltMinimumWindows' '10.0.19045'")
    _find("${_nsis_DEFINES}" "\n  ${_line}\n" _at)
    _check("_at GREATER -1" "CPACK_NSIS_DEFINES lacks '${_line}'")
endforeach()

# First GitBolt's files (the folder's own, all of bin and plugins) in the
# registered folder and $INSTDIR, opened as File opens them but not
# truncated: a sharing or lock violation is Retry/Cancel, under /S the
# in-use exit code. Then the registered uninstaller, only if it is there,
# run from a copy, and installed over if it fails; never a box. Then the
# new one registered before any file, so an install that stops halfway can
# be repaired.
set(_order
    "ReadRegStr $0 HKLM '${_key}' 'UninstallString'"
    "StrCmp $1 '\"' 0 +2"
    "StrCpy $1 $0 '' -14"
    "StrCmp $1 '\\Uninstall.exe' 0 gitbolt_unregistered"
    "IfFileExists $0 0 gitbolt_unregistered"
    "gitbolt_check_in_use:"
    "StrCpy $5 $1"
    "FindFirst $3 $4 '$5\\*.*'"
    "IfFileExists '$6\\*.*' 0 gitbolt_probe"
    "IntCmp $9 0 0 gitbolt_open_subfolder gitbolt_open_subfolder"
    "StrCmp $4 'bin' gitbolt_open_subfolder"
    "StrCmp $4 'plugins' gitbolt_open_subfolder gitbolt_skip_entry"
    "StrCmp $6 $EXEPATH gitbolt_skip_entry"
    "System::Call 'kernel32::CreateFile(t r6, i 0x40000000, i 1, p 0, i 3, i 0, p 0) p .r7 ?e'"
    "StrCmp $8 32 gitbolt_in_use"
    "StrCmp $8 33 gitbolt_in_use"
    "IfFileExists '$INSTDIR\\Uninstall.exe' gitbolt_walk_instdir"
    "IfFileExists '$INSTDIR\\bin\\GitBolt.exe' 0 gitbolt_free"
    "StrCpy $5 $INSTDIR"
    "MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION 'GitBolt is running, or another program is using $6. Close it, then click Retry.' /SD IDCANCEL IDRETRY gitbolt_check_in_use"
    "gitbolt_free:"
    "StrCmp $0 '' gitbolt_uninstalled"
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
# Cancel, or /S: the error level set just before Abort, which makes it the
# exit code instead of 2 (NSIS's Main.c).
string(REGEX MATCH "\n[ \t]*MessageBox [^\n]* /SD IDCANCEL IDRETRY gitbolt_check_in_use\n[ \t]*SetErrorLevel ([0-9]+)\n[ \t]*Abort '[^'\n]*'\n"
       _match "${_nsis_EXTRA_PREINSTALL_COMMANDS}")
if(NOT _in_use_exit STREQUAL "")
    _check("\"${CMAKE_MATCH_1}\" STREQUAL \"${_in_use_exit}\""
           "CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS: the in-use box isn't followed by SetErrorLevel ${_in_use_exit} and Abort")
endif()
# Nothing changed until the check passes, so stopping there leaves all as it was.
_find("${_nsis_EXTRA_PREINSTALL_COMMANDS}" "gitbolt_free:\n" _free)
if(_free GREATER -1)
    string(SUBSTRING "${_nsis_EXTRA_PREINSTALL_COMMANDS}" 0 ${_free} _text)
    string(REPLACE ";" "\\;" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^[ \t]*(CopyFiles|Exec|WriteUninstaller|WriteReg|WriteINI|DeleteReg|Delete|RMDir|Rename|File[ \t]|SetOutPath|CreateDirectory|CreateShortCut|SetFileAttributes)")
            _check("FALSE" "CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS changes something before the in-use check passes: '${_line}'")
        endif()
    endforeach()
endif()

# The uninstaller opened and measured against a fresh copy, not found by
# IfFileExists (which a file deleted while held still passes), and written
# again until it matches; then what Installed apps shows.
set(_order
    "WriteRegStr HKCR 'Directory\\shell\\gitbolt\\command' '' '\"$INSTDIR\\bin\\GitBolt.exe\" \"%V\"'"
    "WriteUninstaller '$PLUGINSDIR\\gitbolt-uninstaller.exe'"
    "FileSeek $0 0 END $3"
    "gitbolt_check_uninstaller:"
    "FileOpen $0 '$INSTDIR\\Uninstall.exe' r"
    "IfErrors gitbolt_rewrite_uninstaller"
    "FileSeek $0 0 END $1"
    "StrCmp $1 $3 gitbolt_have_uninstaller"
    "gitbolt_rewrite_uninstaller:"
    "Sleep $4"
    "WriteUninstaller '$INSTDIR\\Uninstall.exe'"
    "Goto gitbolt_check_uninstaller"
    "Abort 'Could not create the uninstaller. Run the installer again.'"
    "gitbolt_have_uninstaller:"
    "WriteRegStr SHCTX '${_key}' 'InstallLocation' '$INSTDIR'"
    "\${GetSize} '$INSTDIR' '/S=0K' $0 $1 $2"
    "WriteRegDWORD SHCTX '${_key}' 'EstimatedSize' $0")
set(_last -1)
foreach(_line IN LISTS _order)
    _find("${_nsis_EXTRA_INSTALL_COMMANDS}" "${_line}\n" _at)
    _check("_at GREATER _last" "CPACK_NSIS_EXTRA_INSTALL_COMMANDS lacks '${_line}', or has it out of order")
    set(_last ${_at})
endforeach()
_find("${_nsis_EXTRA_INSTALL_COMMANDS}" "IfFileExists" _at)
_check("_at EQUAL -1" "CPACK_NSIS_EXTRA_INSTALL_COMMANDS checks the uninstaller with IfFileExists, which a file deleted while held open passes")

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

# A relative jump (+N) counts instructions: each line it passes over must
# be one, not a label, plugin call or macro, which compile to other counts.
foreach(_name IN LISTS _fragments)
    string(REPLACE ";" "\\;" _text "${_nsis_${_name}}")
    string(REPLACE "\n" ";" _lines "${_text}")
    list(LENGTH _lines _count)
    set(_next 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _next "${_next} + 1")
        if(_line MATCHES "^[ \t]*(StrCmp|StrCmpS|IntCmp|IntCmpU|IfErrors|IfFileExists|IfSilent|Goto) .*[ \t]\\+([0-9]+)$")
            math(EXPR _landing "${_next} + ${CMAKE_MATCH_2} - 1")
            if(_landing GREATER_EQUAL _count)
                _check("FALSE" "${_name}: '${_line}' jumps past the end")
            elseif(_landing GREATER _next)
                math(EXPR _passed "${_landing} - 1")
                foreach(_at RANGE ${_next} ${_passed})
                    list(GET _lines ${_at} _over)
                    if(_over MATCHES ":[ \t]*$|::|\\$\\{|^[ \t]*(!insertmacro|File )|^[ \t]*$")
                        _check("FALSE" "${_name}: '${_line}' jumps over '${_over}', which isn't one instruction")
                    endif()
                endforeach()
            endif()
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
