# ============================================================================
# libgit2 -> the gitbolt_libgit2 interface target
#
# Everything that needs libgit2 links gitbolt_libgit2 (src/git, src/app;
# the tests through gitbolt_git), never a provider's own target.
# GitBolt uses libgit2 only for local repository access; clone, fetch,
# pull and push run the git CLI. So any libgit2 >= 1.7 will do, as long
# as it is thread-safe (test_libgit2_features).
#
# Default: the system copy, via pkg-config (Homebrew, distro packages,
# vcpkg + pkgconf), then libgit2's own CMake package (libgit2 >= 1.8,
# vcpkg). With GITBOLT_BUNDLED_LIBGIT2 -- or when neither is found --
# the release pinned below is downloaded, built static and linked into
# the executables. That is what the macOS and Windows installers ship:
# Homebrew's bottles target the build machine's macOS, and vcpkg's
# libgit2 would put git2, pcre and zlib DLLs into the installer.
# ============================================================================

# GitHub's source tarball for the tag; bump the version and the hash
# together (shasum -a 256 of the downloaded .tar.gz).
set(GITBOLT_LIBGIT2_VERSION 1.9.7)
set(GITBOLT_LIBGIT2_SHA256
    1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7)

add_library(gitbolt_libgit2 INTERFACE)

# A function, so the variables below that configure libgit2 end with it
# instead of leaking into our own directory scope.
function(gitbolt_add_bundled_libgit2)
    include(FetchContent)
    # SOURCE_SUBDIR names a directory that doesn't exist, so
    # MakeAvailable only downloads; the add_subdirectory() below adds it
    # EXCLUDE_FROM_ALL, which also keeps libgit2's install() rules
    # (headers, libgit2.a, pkg-config and CMake files) out of our
    # install tree and so out of every package. Offline:
    # -DFETCHCONTENT_SOURCE_DIR_LIBGIT2=<unpacked v1.9.7 source>, used
    # as is (PatchLibgit2.cmake, which matters only for MSVC, isn't run).
    # The patch script's hash is part of the command, so editing the
    # script re-runs it in existing build directories too.
    set(_patch ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PatchLibgit2.cmake)
    file(SHA256 ${_patch} _patch_sha256)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_patch})
    FetchContent_Declare(libgit2
        URL https://github.com/libgit2/libgit2/archive/refs/tags/v${GITBOLT_LIBGIT2_VERSION}.tar.gz
        URL_HASH SHA256=${GITBOLT_LIBGIT2_SHA256}
        PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH_SHA256=${_patch_sha256} -P ${_patch}
        SOURCE_SUBDIR gitbolt-download-only
    )
    # Bundled means bundled, even with FETCHCONTENT_TRY_FIND_PACKAGE_MODE
    # set to ALWAYS.
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
    FetchContent_MakeAvailable(libgit2)

    # libgit2 declares cmake_minimum_required(3.5.1), so policies newer
    # than that are unset in its tree. These defaults make its option()s
    # (CMP0077) and set(CACHE)s (CMP0126) honour the normal variables
    # below instead of creating cache entries in our cache (BUILD_TESTS,
    # BUILD_SHARED_LIBS, ...) or overriding us, and keep MSVC's /W3 out of
    # its default C flags (CMP0092) so the /w below doesn't trip D9025.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0092 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    # CMake >= 3.31 warns that 3.5.1 compatibility is going away. CMake
    # 4 can instead treat that cmake_minimum_required as 3.10, which
    # silences it; the 3.6-3.10 policies (try_compile flags, IPO, RPATH
    # details) leave libgit2's configure results and build unchanged.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    # Ours (root CMakeLists.txt, qt_standard_project_setup()) and
    # inherited by libgit2's directories: they would give every libgit2
    # target an _autogen step and a C++ mocs_compilation.cpp object.
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTOUIC OFF)
    set(CMAKE_AUTORCC OFF)
    list(APPEND CMAKE_MESSAGE_INDENT "[libgit2] ")

    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_CLI OFF)
    set(BUILD_EXAMPLES OFF)
    # GitBolt calls libgit2 from worker threads. On by default; set
    # anyway, as the one libgit2 feature GitBolt depends on.
    set(USE_THREADS ON)
    # No network operation goes through libgit2 (see the top), so no SSH
    # transport. HTTPS stays: on the OS's own TLS stack it costs no
    # dependency to build or ship.
    set(USE_SSH OFF)
    if(APPLE)
        set(USE_HTTPS SecureTransport)
    elseif(WIN32)
        set(USE_HTTPS WinHTTP)
    else()
        set(USE_HTTPS ON)
    endif()
    set(USE_HTTP_PARSER builtin)
    set(REGEX_BACKEND builtin)
    set(USE_GSSAPI OFF)
    if(WIN32)
        # Windows has no system zlib; don't let a vcpkg one (a DLL) in.
        set(USE_BUNDLED_ZLIB ON)
    endif()
    if(MSVC)
        # libgit2 defaults to the static CRT (/MT); Qt and GitBolt use /MD.
        set(STATIC_CRT OFF)
    endif()

    add_subdirectory(${libgit2_SOURCE_DIR} ${libgit2_BINARY_DIR} EXCLUDE_FROM_ALL)

    # Its backend selectors are set(CACHE) entries: created (empty) even
    # though the variables above win. Hide them, since editing them in
    # ccmake/cmake-gui would change nothing.
    foreach(_var USE_SSH USE_HTTPS USE_HTTP_PARSER REGEX_BACKEND)
        if(DEFINED CACHE{${_var}})
            set_property(CACHE ${_var} PROPERTY TYPE INTERNAL)
        endif()
    endforeach()

    # Third-party C: our build is warning-free, and its warnings aren't
    # ours to fix. Every target libgit2 adds (itself plus bundled pcre2,
    # llhttp, zlib, ntlmclient, xdiff objects) compiles with warnings off.
    set(_dirs ${libgit2_SOURCE_DIR})
    while(_dirs)
        list(POP_FRONT _dirs _dir)
        get_property(_targets DIRECTORY ${_dir} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t IN LISTS _targets)
            get_target_property(_type ${_t} TYPE)
            if(_type MATCHES "^(STATIC|OBJECT)_LIBRARY$")
                target_compile_options(${_t} PRIVATE
                    $<$<COMPILE_LANGUAGE:C>:$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>>)
            endif()
        endforeach()
        get_property(_subdirs DIRECTORY ${_dir} PROPERTY SUBDIRECTORIES)
        list(APPEND _dirs ${_subdirs})
    endwhile()

    # libgit2package (output name git2) carries the system libraries its
    # backends need (Security/CoreFoundation/iconv/z on macOS; winhttp,
    # crypt32, rpcrt4, ole32, secur32, ws2_32 on Windows) as its link
    # interface, but its headers only as an install-time interface.
    # SYSTEM, like any installed libgit2's headers.
    target_link_libraries(gitbolt_libgit2 INTERFACE libgit2package)
    target_include_directories(gitbolt_libgit2 SYSTEM INTERFACE
        ${libgit2_SOURCE_DIR}/include)
endfunction()

set(GITBOLT_LIBGIT2_PROVIDER "")
if(NOT GITBOLT_BUNDLED_LIBGIT2)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBGIT2 IMPORTED_TARGET libgit2)
    endif()
    if(TARGET PkgConfig::LIBGIT2)
        target_link_libraries(gitbolt_libgit2 INTERFACE PkgConfig::LIBGIT2)
        set(GITBOLT_LIBGIT2_PROVIDER "pkg-config")
    else()
        find_package(libgit2 CONFIG QUIET)
        if(TARGET libgit2::libgit2package)
            target_link_libraries(gitbolt_libgit2 INTERFACE libgit2::libgit2package)
            set(GITBOLT_LIBGIT2_PROVIDER "CMake package")
        else()
            message(STATUS "No system libgit2 found; using the bundled one")
        endif()
    endif()
endif()
if(NOT GITBOLT_LIBGIT2_PROVIDER)
    gitbolt_add_bundled_libgit2()
    set(GITBOLT_LIBGIT2_PROVIDER "bundled")
    message(STATUS "libgit2: bundled ${GITBOLT_LIBGIT2_VERSION} (static)")
else()
    message(STATUS "libgit2: system (${GITBOLT_LIBGIT2_PROVIDER})")
endif()
