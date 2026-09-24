cmake_minimum_required(VERSION 3.21)

# Link each static library once. Under the OLD behaviour CMake repeats
# static libraries on link lines, and Apple's linker (Xcode 15+) warns
# "ignoring duplicate libraries" for every test binary. NEW only drops
# the repeats on linkers that don't need them (not GNU ld).
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()
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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)

if(GITBOLT_SANITIZERS)
    include(Sanitizers)
endif()

# Find Qt 6 (Network is for the env-gated agent/test bridge's
# QLocalServer — see docs/AGENT_TESTING.md)
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Svg Network)
qt_standard_project_setup()

# Find libgit2
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBGIT2 IMPORTED_TARGET libgit2)
endif()

if(NOT LIBGIT2_FOUND)
    find_package(libgit2 QUIET)
    if(NOT libgit2_FOUND)
        include(FetchContent)
        message(STATUS "libgit2 not found, fetching from source...")
        FetchContent_Declare(libgit2
            GIT_REPOSITORY https://github.com/libgit2/libgit2.git
            GIT_TAG v1.8.4
        )
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_CLI OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(libgit2)
    endif()
endif()

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
