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
# minos of the Qt 6.10 frameworks CI ships (aqt), so nothing in the
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
project(GitBolt VERSION 0.9.0 LANGUAGES CXX)

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
option(GITBOLT_BUNDLED_LIBGIT2
    "Build the pinned libgit2 from source and link it statically instead of using a system copy"
    OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)

if(GITBOLT_SANITIZERS)
    include(Sanitizers)
endif()

# Find Qt 6 (Network is for the env-gated agent/test bridge's
# QLocalServer — see docs/AGENT_TESTING.md)
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Svg Network)
qt_standard_project_setup()

# libgit2 -> the gitbolt_libgit2 target: a system copy, or the pinned
# static build (GITBOLT_BUNDLED_LIBGIT2, or when there is no system one)
include(Libgit2)

# Source modules
add_subdirectory(src)

# Tests
if(GITBOLT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

# Packaging
include(Packaging)
