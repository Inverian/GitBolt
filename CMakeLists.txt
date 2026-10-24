cmake_minimum_required(VERSION 3.21)

# Link each static library once. Under the OLD behaviour CMake repeats
# static libraries on link lines, and Apple's linker (Xcode 15+) warns
# "ignoring duplicate libraries" for every test binary. NEW only drops
# the repeats on linkers that don't need them (not GNU ld).
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()
# The bundled libgit2 is a URL download: give extracted files the
# extraction time, so a re-download always rebuilds what changed.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Oldest macOS the app runs on. Unset, the binary targets the build
# machine's SDK (macOS 26 on CI), whatever Info.plist says. 13.0 is the
# minos of the Qt 6.11 frameworks CI ships (aqt), so nothing in the
# bundle needs more; LSMinimumSystemVersion is generated from this same
# variable (src/app/platform/macos/Info.plist.in). Must precede
# project(), whose compiler checks use it. CMAKE_HOST_APPLE because
# APPLE is only settled by project(). Empty counts as unset: it is what
# build directories configured before this default have cached. A
# MACOSX_DEPLOYMENT_TARGET environment variable still wins, as it does
# for CMake's own default.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(_gitbolt_macos_min "$ENV{MACOSX_DEPLOYMENT_TARGET}")
    if(NOT _gitbolt_macos_min)
        set(_gitbolt_macos_min 13.0)
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET ${_gitbolt_macos_min} CACHE STRING
        "Minimum macOS version (also the bundle's LSMinimumSystemVersion)" FORCE)
endif()

# vcpkg (Windows): also copy the DLLs our vcpkg deps need (git2.dll,
# pcre.dll, z.dll with vcpkg's libgit2) next to the INSTALLED
# GitBolt.exe, i.e. into CPack's NSIS staging tree; windeployqt deploys
# only Qt's own DLLs. No effect without the vcpkg toolchain file, and
# nothing to copy with GITBOLT_BUNDLED_LIBGIT2 (static). Must precede
# project(), where the toolchain is loaded. An existing build dir keeps
# a cached OFF from before until it is configured fresh.
set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON CACHE BOOL
    "Copy vcpkg runtime DLLs into the install tree (NSIS payload)")
project(GitBolt VERSION 0.9.8 LANGUAGES CXX)
# Objective-C++ for the few AppKit calls Qt has no API for
# (src/app/platform/macos/MacServices.mm). It takes CXX_STANDARD.
if(APPLE)
    enable_language(OBJCXX)
endif()

# An empty build type builds with NO optimization and no debug info —
# the worst of both worlds, and exactly what `cmake -B build` with no
# -DCMAKE_BUILD_TYPE gave you. Default to Debug for hacking; release
# builds say -DCMAKE_BUILD_TYPE=Release explicitly (CI and the docs
# both do).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
                 STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# compile_commands.json for clangd / clang-tidy (the checked-in
# .clang-tidy is useless without it).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(GITBOLT_BUILD_TESTS "Build tests" ON)
option(GITBOLT_SANITIZERS "Enable sanitizers (address, undefined)" OFF)
# The pinned libgit2, patched by cmake/PatchLibgit2.cmake against
# repositories that crash or freeze GitBolt, as every installer has it.
# OFF takes a system libgit2, for packagers whose policy is the
# distribution's: it has none of those patches (configure lists them)
# and must be 1.9.5 or later (cmake/Libgit2Floor.cmake). A build
# directory configured before this default keeps its OFF until
# configured with -DGITBOLT_BUNDLED_LIBGIT2=ON or afresh.
option(GITBOLT_BUNDLED_LIBGIT2
    "Build the pinned libgit2, with GitBolt's patches, from source and link it statically; OFF uses a system copy (1.9.5 or later) without them"
    ON)
include(CMakeDependentOption)
cmake_dependent_option(GITBOLT_ALLOW_OLD_SYSTEM_LIBGIT2
    "With GITBOLT_BUNDLED_LIBGIT2=OFF, accept a system libgit2 before 1.9.5 (1.8.6) whose distribution backported the fixes for CVE-2026-53584 and CVE-2026-53585"
    OFF "NOT GITBOLT_BUNDLED_LIBGIT2" OFF)
option(GITBOLT_REQUIRE_ENGLISH_PLURALS
    "Fail the configure when the English plural forms can't be built (no Qt LinguistTools, or Qt before 6.7)"
    OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)
# Usage statistics endpoints: the GITBOLT_TELEMETRY_* cache entries.
# Empty, the default, compiles telemetry out.
include(Telemetry)

if(GITBOLT_SANITIZERS)
    include(Sanitizers)
endif()

# Find Qt 6 (Network: the env-gated agent/test bridge's QLocalServer —
# see docs/AGENT_TESTING.md — and the usage statistics client's HTTPS,
# src/telemetry)
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Svg Network)
qt_standard_project_setup()

# libgit2 -> the gitbolt_libgit2 target: the pinned, patched static
# build (GITBOLT_BUNDLED_LIBGIT2, the default), or a system copy
include(Libgit2)

# Source modules
add_subdirectory(src)

# English plural forms. Qt picks the form of tr("%n file(s)", nullptr,
# n) only from a loaded translation, so GitBolt ships an English one
# holding nothing but the singular and plural of each %n string:
# translations/gitbolt_en.ts, compiled into gitbolt_util (which the app
# and every test link) as :/i18n/gitbolt_en.qm, and loaded by
# util::installEnglishPlurals() (src/util/Translations.h). Optional: it
# needs Qt LinguistTools (in aqt's and Homebrew's Qt; see
# docs/BUILDING.md for distro packages) and Qt 6.7 for PLURALS_TS_FILE.
# Without them the build works and the texts keep their "(s)". CI
# configures with GITBOLT_REQUIRE_ENGLISH_PLURALS=ON, so no installer
# ships without them.
#
# After adding or changing a %n string:
#   cmake --build build --target update_translations
# then fill in both forms of each new <message numerus="yes"> in
# translations/gitbolt_en.ts, drop its type="unfinished", and commit
# the file. CI's Linux leg re-runs update_translations and fails while
# the file is stale or a form is missing.
#
# SOURCE_TARGETS: every module with tr() texts. gitbolt_conpty
# (Windows) is Qt-free. -locations none keeps line numbers out of the
# file, so it only changes when a text does; -no-obsolete drops texts
# the sources no longer have.
find_package(Qt6 QUIET OPTIONAL_COMPONENTS LinguistTools)
if(Qt6LinguistTools_FOUND AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
    # The .qm must sit in gitbolt_util's binary dir: the resource path
    # is the file's path relative to it, under RESOURCE_PREFIX.
    get_target_property(_gitbolt_util_bin gitbolt_util BINARY_DIR)
    qt_add_translations(gitbolt_util
        SOURCE_TARGETS
            gitbolt gitbolt_instance gitbolt_ui gitbolt_dialogs gitbolt_widgets
            gitbolt_editor gitbolt_services gitbolt_models gitbolt_git
            gitbolt_telemetry gitbolt_watcher gitbolt_conf gitbolt_util
        PLURALS_TS_FILE ${CMAKE_SOURCE_DIR}/translations/gitbolt_en.ts
        RESOURCE_PREFIX /i18n
        QM_OUTPUT_DIRECTORY ${_gitbolt_util_bin}
        LUPDATE_OPTIONS -locations none -no-obsolete)
    target_compile_definitions(gitbolt_util PUBLIC GITBOLT_ENGLISH_PLURALS)
    # lupdate reads every source of a target and took the app's .qrc
    # for C++ ("Unterminated C++ character"). Only C++ has texts here.
    get_target_property(_gitbolt_app_sources gitbolt SOURCES)
    list(FILTER _gitbolt_app_sources EXCLUDE REGEX "\\.(cpp|h)$")
    set_property(TARGET gitbolt APPEND PROPERTY
        QT_EXCLUDE_SOURCES_FROM_TRANSLATION ${_gitbolt_app_sources})
    message(STATUS "English plural forms: on (Qt ${Qt6_VERSION} LinguistTools)")
elseif(GITBOLT_REQUIRE_ENGLISH_PLURALS)
    if(Qt6LinguistTools_FOUND)
        set(_gitbolt_linguist "found")
    else()
        set(_gitbolt_linguist "not found")
    endif()
    message(FATAL_ERROR "GITBOLT_REQUIRE_ENGLISH_PLURALS: the English plural forms need "
        "Qt LinguistTools and Qt 6.7 or later (Qt ${Qt6_VERSION}, LinguistTools "
        "${_gitbolt_linguist})")
else()
    message(STATUS "English plural forms: off (needs Qt LinguistTools and Qt 6.7+); "
        "texts with a count read \"1 file(s)\"")
endif()

# Tests
if(GITBOLT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

# Packaging
include(Packaging)
