set(GITBOLT_APP_SOURCES
    main.cpp
    TestBridge.cpp
    TestBridge.h
    resources/gitbolt.qrc
)

# On macOS, bundle the .icns so the Dock and Finder show our app icon.
# The source-file property MACOSX_PACKAGE_LOCATION places the file under
# GitBolt.app/Contents/Resources/, where CFBundleIconFile expects it.
# Pulled directly from the canonical resources/icons/ folder — when
# that file changes, CMake re-copies it into the bundle on next build.
if(APPLE)
    set(GITBOLT_MACOS_ICON "${CMAKE_SOURCE_DIR}/resources/icons/gitbolt.icns")
    set_source_files_properties(${GITBOLT_MACOS_ICON} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND GITBOLT_APP_SOURCES ${GITBOLT_MACOS_ICON})
endif()

# On Windows, embed the .ico into the .exe so Explorer, the Start
# menu, the Alt+Tab switcher, and the taskbar all show our icon.
#
# The mechanism: we copy the canonical resources/icons/gitbolt.ico
# into the build tree via configure_file(COPYONLY) — which also sets
# up CMake's dependency tracking, so a change to the canonical icon
# causes the exe to rebuild on the next build. Then we materialize a
# .rc file next to the copy (same directory so the resource compiler
# finds the .ico via its relative reference) and add it to the target
# sources. CMake automatically hands .rc files to the platform's
# resource compiler (rc.exe on MSVC, windres on MinGW).
if(WIN32)
    configure_file(
        "${CMAKE_SOURCE_DIR}/resources/icons/gitbolt.ico"
        "${CMAKE_CURRENT_BINARY_DIR}/gitbolt.ico"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/gitbolt.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/gitbolt.rc"
        COPYONLY
    )
    list(APPEND GITBOLT_APP_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/gitbolt.rc")
endif()

qt_add_executable(gitbolt ${GITBOLT_APP_SOURCES})
gitbolt_set_warnings(gitbolt)

target_link_libraries(gitbolt PRIVATE
    gitbolt_ui
    gitbolt_dialogs
    gitbolt_widgets
    gitbolt_editor
    gitbolt_services
    gitbolt_models
    gitbolt_git
    gitbolt_conf
    gitbolt_watcher
    gitbolt_util
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
)

# libgit2 for git_libgit2_init / git_libgit2_shutdown in main
if(TARGET PkgConfig::LIBGIT2)
    target_link_libraries(gitbolt PRIVATE PkgConfig::LIBGIT2)
elseif(TARGET libgit2::git2)
    target_link_libraries(gitbolt PRIVATE libgit2::git2)
elseif(TARGET git2)
    target_link_libraries(gitbolt PRIVATE git2)
endif()

set_target_properties(gitbolt PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "GitBolt"
)

# Linux executables are lowercase by convention, and gitbolt.desktop
# (Exec=gitbolt), the Nautilus extension and the AppStream <binary> all
# run `gitbolt`; linuxdeploy rejects an Exec= that names no file in
# usr/bin (the match is case-sensitive).
if(UNIX AND NOT APPLE)
    set_target_properties(gitbolt PROPERTIES OUTPUT_NAME "gitbolt")
endif()

if(APPLE)
    set_target_properties(gitbolt PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.gitbolt.app"
        MACOSX_BUNDLE_BUNDLE_NAME "GitBolt"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE "gitbolt.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in"
    )
endif()

# Single source of truth for the user-visible version string
# (About dialog, --version): the project() version above CPack's
# package stamping. main.cpp once hardcoded its own copy.
target_compile_definitions(gitbolt PRIVATE
    GITBOLT_VERSION="${PROJECT_VERSION}")

# Bundle the Qt runtime into the INSTALLED app. This runs at
# `cmake --install` time — and therefore also inside CPack's own
# staging install, which is what actually ends up in the DMG / NSIS /
# DEB payload. Running a deploy tool by hand on some other install tree
# (as CI used to) never affects what CPack ships.
if(APPLE OR WIN32)
    install(TARGETS gitbolt
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    qt_generate_deploy_app_script(
        TARGET gitbolt
        OUTPUT_SCRIPT gitbolt_deploy_script
        # windeployqt would otherwise copy the ~25 MB vc_redist.x64.exe
        # into bin/ for Release builds, and nothing runs it. The MSVC
        # runtime DLLs go in app-local via InstallRequiredSystemLibraries
        # (cmake/Packaging.cmake). Only affects the windeployqt branch.
        NO_COMPILER_RUNTIME
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${gitbolt_deploy_script})
else()
    # Linux: self-contained like the macOS bundle and the Windows dir,
    # because no distro Qt is new enough (Ubuntu 24.04 ships 6.4). The
    # real binary, a private Qt and its plugins live in lib/gitbolt, with
    # a bin/gitbolt symlink (DEB: /usr/lib/gitbolt + /usr/bin/gitbolt).
    # Never let the private Qt reach <prefix>/lib (the shared linker path)
    # or qt.conf reach <prefix>/bin (every Qt app in /usr/bin would read
    # it), which is what the default deploy layout would do.
    set(GITBOLT_PRIVATE_DIR "lib/gitbolt")
    set_target_properties(gitbolt PROPERTIES INSTALL_RPATH "\$ORIGIN/lib")
    install(TARGETS gitbolt RUNTIME DESTINATION "${GITBOLT_PRIVATE_DIR}")
    # EXECUTABLE is the BUILD-tree binary, as in Qt's own Linux deploy
    # script: file(GET_RUNTIME_DEPENDENCIES) resolves only through
    # RPATH/RUNPATH, the ldconfig dirs and DIRECTORIES (never
    # LD_LIBRARY_PATH), and only the build RUNPATH points at the Qt
    # install. QT_DEPLOY_LIB_DIR (not a LIB_DIR argument) is what Qt uses
    # to compute each plugin's new RUNPATH, so it names the private lib
    # dir (plugins get $ORIGIN/../../lib). EXCLUDE_PLUGIN_TYPES: evdev/
    # libinput, eglfs/KMS and GTK-theme plugins pull system libraries a
    # desktop Git client doesn't need. NO_TRANSLATIONS: the default target
    # would be <prefix>/translations.
    qt_generate_deploy_script(
        TARGET gitbolt
        OUTPUT_SCRIPT gitbolt_deploy_script
        CONTENT "
set(QT_DEPLOY_BIN_DIR \"${GITBOLT_PRIVATE_DIR}\")
set(QT_DEPLOY_LIB_DIR \"${GITBOLT_PRIVATE_DIR}/lib\")
set(QT_DEPLOY_PLUGINS_DIR \"${GITBOLT_PRIVATE_DIR}/plugins\")
qt_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE:gitbolt>\"
    EXCLUDE_PLUGIN_TYPES generic egldeviceintegrations platformthemes
    GENERATE_QT_CONF
    NO_TRANSLATIONS
)
")
    install(SCRIPT "${gitbolt_deploy_script}")
    file(CREATE_LINK "../${GITBOLT_PRIVATE_DIR}/gitbolt"
         "${CMAKE_CURRENT_BINARY_DIR}/gitbolt-bin-link" SYMBOLIC)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/gitbolt-bin-link"
            DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME gitbolt)
endif()

# macdeployqt strips every framework and dylib it copies into the bundle
# (Qt, plus Homebrew's libgit2 and its deps) and rewrites their install
# names, which invalidates their signatures. Qt 6.10's macdeployqt does
# not re-sign (ad-hoc signing became its default only in 6.11,
# QTBUG-138019), so the bundle fails `codesign --verify` and Gatekeeper
# calls a downloaded copy "damaged". Re-sign ad-hoc after the deploy
# script; this also runs inside CPack's staging install, so it is what
# the DMG ships. Fail the install if the result doesn't verify.
# "GitBolt.app" = OUTPUT_NAME above, installed to BUNDLE DESTINATION ".".
if(APPLE)
    install(CODE [[
        set(_gitbolt_app "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/GitBolt.app")
        message(STATUS "Ad-hoc signing ${_gitbolt_app}")
        execute_process(
            COMMAND codesign --force --deep --sign - "${_gitbolt_app}"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(
            COMMAND codesign --verify --deep --strict "${_gitbolt_app}"
            COMMAND_ERROR_IS_FATAL ANY)
    ]])
endif()
