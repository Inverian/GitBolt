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

install(TARGETS gitbolt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Bundle the Qt runtime into the INSTALLED app. This runs at
# `cmake --install` time — and therefore also inside CPack's own
# staging install, which is what actually ends up in the DMG/NSIS
# payload. Running macdeployqt/windeployqt by hand on some other
# install tree (as CI used to) never affects what CPack ships.
# Linux stays on system Qt: the DEB declares shared-lib deps and the
# AppImage is assembled by linuxdeploy in CI.
if(APPLE OR WIN32)
    qt_generate_deploy_app_script(
        TARGET gitbolt
        OUTPUT_SCRIPT gitbolt_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${gitbolt_deploy_script})
endif()
