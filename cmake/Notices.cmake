# ============================================================================
# The licence texts every package ships (cmake/Packaging.cmake, the app's
# bundle in src/app/CMakeLists.txt): GitBolt's own GPL-3.0 (LICENSE), and
# THIRD-PARTY-NOTICES.txt for the software of others a package holds:
#   - Qt, under the LGPL-3.0, and the code of others in Qt's libraries
#     (resources/licenses/qt-<Qt version>-third-party.txt, written by
#     tools/update-qt-notices.py);
#   - the bundled libgit2: its COPYING, and the notices of the code in it
#     that COPYING leaves out (libgit2-<version>-extra.txt, with the LGPL-2.1
#     for LibXDiff);
#   - ICU, on Linux, when Qt's own lib folder has it (aqt's Qt does, and the
#     deploy copies it);
#   - Google's Material Symbols, GitBolt's menu icons (Apache-2.0);
#   - on Windows, Microsoft's C++ runtime and NSIS (the installer, and the
#     Uninstall.exe it leaves).
# On Linux all of it goes in one file, share/doc/gitbolt/copyright, as Debian
# has it. resources/licenses/SOURCES.md says where each text comes from.
#
# A text that should be there and isn't is a warning, so a build against
# another Qt (Homebrew's, a distribution's) still configures; with
# GITBOLT_STRICT_NOTICES, which CI sets, it fails the configure, so no
# package ships without it: a Qt, libgit2 or ICU update needs its texts
# first.
#
# Written at configure time, from Qt6_VERSION and the libgit2 source that
# cmake/Libgit2.cmake fetched, so include this after both. A file is only
# rewritten when its text changes, so a configure doesn't rebuild the
# bundle each time.
#
#   GITBOLT_NOTICES_FILE    <build>/THIRD-PARTY-NOTICES.txt
#   GITBOLT_COPYRIGHT_FILE  <build>/copyright (LICENSE + the notices)
# ============================================================================

option(GITBOLT_STRICT_NOTICES
    "Fail the configure when a licence text the packages need is missing from resources/licenses (CI sets it)"
    OFF)

set(_gitbolt_licenses "${CMAKE_SOURCE_DIR}/resources/licenses")
set(_gitbolt_rule "================================================================================")
string(REGEX MATCH "^[0-9]+\\.[0-9]+" _gitbolt_qt_minor "${Qt6_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/LICENSE")
# A text added to resources/licenses after a configure (the one a Qt or ICU
# update needs) configures again too, not only a change to one read here.
file(GLOB _gitbolt_license_files CONFIGURE_DEPENDS "${_gitbolt_licenses}/*")

function(_gitbolt_notice_missing what)
    if(GITBOLT_STRICT_NOTICES)
        message(FATAL_ERROR "cmake/Notices.cmake: ${what} (GITBOLT_STRICT_NOTICES)")
    else()
        message(WARNING "cmake/Notices.cmake: ${what}; the packages of this build would ship without it")
    endif()
endfunction()

# Appends <file>'s text under <heading> to _gitbolt_texts. A macro, so the
# text lands in the caller's variable.
macro(_gitbolt_notice_text heading file)
    if(EXISTS "${file}")
        file(READ "${file}" _gitbolt_text)
        string(APPEND _gitbolt_texts
            "\n${_gitbolt_rule}\n${heading}\n${_gitbolt_rule}\n\n${_gitbolt_text}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    else()
        _gitbolt_notice_missing("no ${file}")
    endif()
endmacro()

set(_gitbolt_texts "")

# --- Qt -----------------------------------------------------------------
if(WIN32)
    set(_gitbolt_qt_linking
        "  GitBolt links Qt's libraries dynamically (and Qt's EntryPoint, under the\n"
        "  BSD-3-Clause licence, statically): they can be replaced with another build\n"
        "  of the same Qt version.")
else()
    set(_gitbolt_qt_linking
        "  GitBolt links Qt's libraries dynamically: they can be replaced with another\n"
        "  build of the same Qt version.")
endif()
string(CONCAT _gitbolt_index
    "GitBolt ${PROJECT_VERSION} ships the following software of others, each under\n"
    "its own licence; the texts follow this list. GitBolt itself is under the GNU\n"
    "General Public License v3.0 (LICENSE); its source, with the patches it\n"
    "applies to libgit2, is at https://github.com/Inverian/GitBolt.\n"
    "\n"
    "Qt ${Qt6_VERSION}\n"
    "  The Qt Company Ltd. and other contributors, https://www.qt.io\n"
    "  Under the GNU Lesser General Public License v3.0, which adds to the GPL-3.0\n"
    "  in LICENSE.\n"
    ${_gitbolt_qt_linking} "\n"
    "  Source code: https://download.qt.io/archive/qt/${_gitbolt_qt_minor}/${Qt6_VERSION}/single/\n")
if(EXISTS "${_gitbolt_licenses}/qt-${Qt6_VERSION}-third-party.txt")
    string(APPEND _gitbolt_index
        "  Qt's libraries hold code of others in turn, under their own licences: see\n"
        "  \"Qt ${Qt6_VERSION}: code of others in Qt's libraries\" below.\n")
else()
    # At least where to look: the licences page of the same Qt series.
    string(APPEND _gitbolt_index
        "  Qt's libraries hold code of others in turn, under the licences listed at\n"
        "  https://doc.qt.io/qt-${_gitbolt_qt_minor}/licenses-used-in-qt.html\n")
endif()
_gitbolt_notice_text("Qt: GNU Lesser General Public License v3.0"
    "${_gitbolt_licenses}/LGPL-3.0.txt")
_gitbolt_notice_text("Qt ${Qt6_VERSION}: code of others in Qt's libraries"
    "${_gitbolt_licenses}/qt-${Qt6_VERSION}-third-party.txt")

# --- libgit2 ------------------------------------------------------------
if(GITBOLT_BUNDLED_LIBGIT2)
    string(APPEND _gitbolt_index
        "\n"
        "libgit2 ${GITBOLT_LIBGIT2_VERSION}\n"
        "  The libgit2 contributors, https://libgit2.org\n"
        "  Linked into GitBolt, under the GNU General Public License v2 with the\n"
        "  linking exception. Its COPYING also holds the licences of the code it\n"
        "  bundles; the code it holds that COPYING leaves out, such as LibXDiff\n"
        "  (GNU Lesser General Public License v2.1 or later), follows it. Source code:\n"
        "  https://github.com/libgit2/libgit2/archive/refs/tags/v${GITBOLT_LIBGIT2_VERSION}.tar.gz\n")
    _gitbolt_notice_text("libgit2: COPYING" "${libgit2_SOURCE_DIR}/COPYING")
    _gitbolt_notice_text("libgit2: code COPYING leaves out"
        "${_gitbolt_licenses}/libgit2-${GITBOLT_LIBGIT2_VERSION}-extra.txt")
    _gitbolt_notice_text("LibXDiff, in libgit2: GNU Lesser General Public License v2.1"
        "${_gitbolt_licenses}/LGPL-2.1.txt")
else()
    # The system's libgit2, which a package may carry along (macdeployqt,
    # vcpkg's app-local DLLs): where its licence is, at least.
    string(APPEND _gitbolt_index
        "\n"
        "libgit2 (the system's, linked dynamically where a package carries it)\n"
        "  The libgit2 contributors, https://libgit2.org, under the GNU General\n"
        "  Public License v2 with the linking exception:\n"
        "  https://github.com/libgit2/libgit2/blob/main/COPYING\n")
endif()

# --- ICU (Linux) --------------------------------------------------------
# Deployed from Qt's own lib folder (src/app/CMakeLists.txt) when the Qt
# there has it, as aqt's Qt does; a distribution's Qt uses the system's
# ICU, which no package here carries.
if(UNIX AND NOT APPLE)
    get_target_property(_gitbolt_qtcore Qt6::Core LOCATION)
    get_filename_component(_gitbolt_qtlib "${_gitbolt_qtcore}" DIRECTORY)
    file(GLOB _gitbolt_icu LIST_DIRECTORIES false "${_gitbolt_qtlib}/libicuuc.so.*.*")
    if(_gitbolt_icu)
        list(GET _gitbolt_icu 0 _gitbolt_icu)
        string(REGEX MATCH "libicuuc\\.so\\.([0-9]+\\.[0-9]+)" _gitbolt_icu "${_gitbolt_icu}")
        set(_gitbolt_icu_version "${CMAKE_MATCH_1}")
        string(APPEND _gitbolt_index
            "\n"
            "ICU ${_gitbolt_icu_version}\n"
            "  Unicode, Inc. and others, https://icu.unicode.org\n"
            "  The libraries libicuuc, libicui18n and libicudata, from Qt's build, under\n"
            "  the ICU licence and the licences of the data it includes, below.\n")
        _gitbolt_notice_text("ICU ${_gitbolt_icu_version}: LICENSE"
            "${_gitbolt_licenses}/ICU-${_gitbolt_icu_version}.txt")
    endif()
endif()

# --- GitBolt's menu icons -------------------------------------------------
string(APPEND _gitbolt_index
    "\n"
    "Material Symbols\n"
    "  Google LLC, https://fonts.google.com/icons\n"
    "  GitBolt's menu icons, recoloured, under the Apache License 2.0.\n")
_gitbolt_notice_text("Material Symbols" "${_gitbolt_licenses}/material-symbols.txt")
_gitbolt_notice_text("Apache License 2.0" "${_gitbolt_licenses}/Apache-2.0.txt")

# --- Windows: the C++ runtime and the installer ---------------------------
if(WIN32)
    file(GLOB _gitbolt_nsis LIST_DIRECTORIES false "${_gitbolt_licenses}/NSIS-*.txt")
    string(APPEND _gitbolt_index
        "\n"
        "Microsoft Visual C++ runtime\n"
        "  Microsoft Corporation. Its DLLs in bin\\, unmodified, under Microsoft's\n"
        "  terms, below, not under GitBolt's licence.\n"
        "\n"
        "NSIS (Nullsoft Scriptable Install System)\n"
        "  The NSIS contributors, https://nsis.sourceforge.io\n"
        "  GitBolt's installer and the Uninstall.exe it leaves are made with it.\n")
    _gitbolt_notice_text("Microsoft Visual C++ runtime"
        "${_gitbolt_licenses}/windows-runtime.txt")
    list(LENGTH _gitbolt_nsis _gitbolt_count)
    if(_gitbolt_count EQUAL 1)
        _gitbolt_notice_text("NSIS: COPYING" "${_gitbolt_nsis}")
    else()
        _gitbolt_notice_missing("not one NSIS-<version>.txt in ${_gitbolt_licenses} (${_gitbolt_count})")
    endif()
endif()

set(_gitbolt_notices "${_gitbolt_index}${_gitbolt_texts}")

file(READ "${CMAKE_SOURCE_DIR}/LICENSE" _gitbolt_text)
string(CONCAT _gitbolt_copyright
    "GitBolt ${PROJECT_VERSION}\n"
    "https://www.gitbolt.com, source at https://github.com/Inverian/GitBolt\n"
    "\n"
    "GitBolt is under the GNU General Public License v3.0, which follows, and\n"
    "ships software of others under their own licences, after it.\n"
    "\n${_gitbolt_rule}\nGitBolt: GNU General Public License v3.0\n${_gitbolt_rule}\n\n"
    "${_gitbolt_text}"
    "\n${_gitbolt_rule}\nSoftware of others\n${_gitbolt_rule}\n\n"
    "${_gitbolt_notices}")

# Left alone while its text is the same, so its time stamp is too.
function(_gitbolt_write_if_changed path text)
    if(EXISTS "${path}")
        file(READ "${path}" _old)
        if(_old STREQUAL text)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${text}")
endfunction()

set(GITBOLT_NOTICES_FILE "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt")
set(GITBOLT_COPYRIGHT_FILE "${CMAKE_BINARY_DIR}/copyright")
_gitbolt_write_if_changed("${GITBOLT_NOTICES_FILE}" "${_gitbolt_notices}")
_gitbolt_write_if_changed("${GITBOLT_COPYRIGHT_FILE}" "${_gitbolt_copyright}")
