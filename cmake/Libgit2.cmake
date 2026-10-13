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
# the executables. That is what every installer ships: Homebrew's
# bottles target the build machine's macOS, vcpkg's libgit2 would put
# git2, pcre and zlib DLLs into the installer, and a distribution's
# libgit2 would make the DEB depend on the one release that packages
# its soname.
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
    # as is: PatchLibgit2.cmake isn't run, so its MSVC changes and its
    # config fixes are missing unless that tree has them (configure
    # warns about the config fixes below; docs/BUILDING.md says how to
    # patch the tree).
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
    # silences it. One of the 3.6-3.10 policies does change a configure
    # result: CMP0067 has try_compile() honour libgit2's C standard,
    # c90 without extensions, and glibc's <sys/stat.h> hides st_mtim
    # under a strict -std=c90. libgit2's FindStatNsec runs before its
    # DefaultCFlags adds the -D_GNU_SOURCE its own sources compile with
    # (every compiler but MSVC), so on Linux it found no nanosecond
    # field, left USE_NSEC on regardless, and the build stopped at
    # "GIT_USE_NSEC defined but unknown struct stat nanosecond type".
    # The checks get the -D_GNU_SOURCE the build has.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    if(NOT MSVC)
        list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    endif()
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
    # transport. HTTPS stays where it's the OS's own TLS stack, which
    # costs no dependency to build or ship. Elsewhere (Linux) it would be
    # OpenSSL: a library linked for nothing, and one more the AppImage
    # bundles. Without it SHA-256 uses libgit2's builtin implementation.
    set(USE_SSH OFF)
    if(APPLE)
        set(USE_HTTPS SecureTransport)
    elseif(WIN32)
        set(USE_HTTPS WinHTTP)
    else()
        set(USE_HTTPS OFF)
        # Its default already, without HTTPS: NTLM takes its crypto
        # from the HTTPS backend.
        set(USE_NTLMCLIENT OFF)
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

    # Whether this tree has PatchLibgit2.cmake's config fixes, the
    # get_entry() crash and the stale config after it (an offline source
    # directory may not): the test that needs them skips without them,
    # and configure says so.
    file(READ ${libgit2_SOURCE_DIR}/src/libgit2/config.c _config_c)
    file(READ ${libgit2_SOURCE_DIR}/src/libgit2/config_file.c _config_file_c)
    if(_config_c MATCHES "GIT_ENOTFOUND\\)[ \t\n]*{[ \t\n]*if \\(res == 0\\)"
       AND _config_file_c MATCHES
           "if \\(error < 0\\)[ \t\n]*git_futils_filestamp_set\\(&b->file\\.stamp, NULL\\)")
        target_compile_definitions(gitbolt_libgit2 INTERFACE GITBOLT_LIBGIT2_CONFIG_PATCHED)
    else()
        message(WARNING "bundled libgit2: ${libgit2_SOURCE_DIR} lacks "
            "cmake/PatchLibgit2.cmake's config fixes, one of them for a crash; "
            "run cmake -P ${_patch} in that directory")
    endif()

    # Say so at configure time, not as an #error deep in the build: a
    # Unix libgit2 with nanosecond times needs one of these fields.
    if(NOT WIN32 AND USE_NSEC AND NOT (HAVE_STRUCT_STAT_ST_MTIM OR HAVE_STRUCT_STAT_ST_MTIMESPEC
                                       OR HAVE_STRUCT_STAT_MTIME_NSEC))
        message(FATAL_ERROR "bundled libgit2: USE_NSEC is on, but no struct stat nanosecond "
            "field was found (st_mtim, st_mtimespec, st_mtime_nsec); see "
            "CMakeConfigureLog.yaml for the checks' compiler flags. A build directory "
            "configured before keeps failed checks cached: configure a fresh one.")
    endif()

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
    message(STATUS "libgit2: system (${GITBOLT_LIBGIT2_PROVIDER}), "
        "without cmake/PatchLibgit2.cmake's config fixes")
endif()
