cmake_minimum_required(VERSION 3.21)
project(GitBolt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Options
option(GITBOLT_BUILD_TESTS "Build tests" ON)
option(GITBOLT_BUILD_PLUGINS "Build plugin system" ON)
option(GITBOLT_SANITIZERS "Enable sanitizers (address, undefined)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)

if(GITBOLT_SANITIZERS)
    include(Sanitizers)
endif()

# Find Qt 6
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Svg)
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
