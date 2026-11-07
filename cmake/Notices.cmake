# ============================================================================
# The licence texts every package ships (cmake/Packaging.cmake, the app's
# bundle in src/app/CMakeLists.txt, the AppImage step in CI): GitBolt's own
# GPL-3.0 (LICENSE), and THIRD-PARTY-NOTICES.txt for what GitBolt ships of
# others: Qt, under the LGPL-3.0, and the bundled libgit2's COPYING, with the
# licences of the code libgit2 bundles. On Linux both go in one file,
# share/doc/gitbolt/copyright, as Debian has it.
#
# Written at configure time, from Qt6_VERSION and the libgit2 source that
# cmake/Libgit2.cmake fetched, so include this after both. A file is only
# rewritten when its text changes, so a configure doesn't rebuild the
# bundle each time.
#
#   GITBOLT_NOTICES_FILE    <build>/THIRD-PARTY-NOTICES.txt
#   GITBOLT_COPYRIGHT_FILE  <build>/copyright (LICENSE + the notices)
# ============================================================================

set(_gitbolt_lgpl "${CMAKE_SOURCE_DIR}/resources/licenses/LGPL-3.0.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/LICENSE" "${_gitbolt_lgpl}")

string(REGEX MATCH "^[0-9]+\\.[0-9]+" _gitbolt_qt_minor "${Qt6_VERSION}")
set(_gitbolt_rule "================================================================================")

string(CONCAT _gitbolt_notices
    "GitBolt ${PROJECT_VERSION} ships the following software of others, each under\n"
    "its own licence. GitBolt itself is under the GNU General Public License v3.0\n"
    "(LICENSE); its source, with the patches it applies to libgit2, is at\n"
    "https://github.com/Inverian/GitBolt.\n"
    "\n"
    "Qt ${Qt6_VERSION}\n"
    "  The Qt Company Ltd. and other contributors, https://www.qt.io\n"
    "  Under the GNU Lesser General Public License v3.0, below, which adds to the\n"
    "  GPL-3.0 in LICENSE. GitBolt links Qt's libraries dynamically: they can be\n"
    "  replaced with another build of the same Qt version. Source code:\n"
    "  https://download.qt.io/archive/qt/${_gitbolt_qt_minor}/${Qt6_VERSION}/single/\n"
    "  Qt's libraries hold code of others in turn, under the licences listed at\n"
    "  https://doc.qt.io/qt-6/licenses-used-in-qt.html\n")

if(GITBOLT_BUNDLED_LIBGIT2)
    string(APPEND _gitbolt_notices
        "\n"
        "libgit2 ${GITBOLT_LIBGIT2_VERSION}\n"
        "  The libgit2 contributors, https://libgit2.org\n"
        "  Linked into GitBolt, under the GNU General Public License v2 with the\n"
        "  linking exception. Its COPYING, below, also holds the licences of the code\n"
        "  it bundles. Source code:\n"
        "  https://github.com/libgit2/libgit2/archive/refs/tags/v${GITBOLT_LIBGIT2_VERSION}.tar.gz\n")
endif()

file(READ "${_gitbolt_lgpl}" _gitbolt_text)
string(APPEND _gitbolt_notices
    "\n${_gitbolt_rule}\nQt: GNU Lesser General Public License v3.0\n${_gitbolt_rule}\n\n"
    "${_gitbolt_text}")
if(GITBOLT_BUNDLED_LIBGIT2)
    file(READ "${libgit2_SOURCE_DIR}/COPYING" _gitbolt_text)
    string(APPEND _gitbolt_notices
        "\n${_gitbolt_rule}\nlibgit2: COPYING\n${_gitbolt_rule}\n\n${_gitbolt_text}")
endif()

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
