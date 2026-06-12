# ============================================================================
# GitBolt CPack Packaging Configuration
# Supports: DMG (macOS), NSIS (Windows), AppImage/DEB (Linux)
# ============================================================================

set(CPACK_PACKAGE_NAME "GitBolt")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "GitBolt")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast cross-platform Git GUI client")
set(CPACK_PACKAGE_DESCRIPTION "GitBolt is a high-performance Git GUI client built with Qt 6 and libgit2.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ThePieMonster/GitBolt")
set(CPACK_PACKAGE_CONTACT "GitBolt Team <team@gitbolt.dev>")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "GitBolt")

# ============================================================================
# macOS — DMG installer with app bundle
# ============================================================================
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "GitBolt")
    set(CPACK_DMG_FORMAT "UDBZ")  # bzip2 compressed
    # Do NOT bake the LICENSE in as a DMG click-through SLA: Apple
    # deprecated DMG SLAs, every scripted `hdiutil attach` (CI, this
    # repo's own smoke test) hangs on the agreement prompt, and users
    # get a wall of license text before they can drag the app. The
    # license still ships inside the bundle and for generators where
    # an agreement step is normal (NSIS).
    set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
    # The DragNDrop generator adds the /Applications symlink itself, so
    # the bundle sits at the DMG root with no install-prefix override.
    # Qt is deployed into the bundle during CPack's staging install via
    # the qt_generate_deploy_app_script hook in src/app/CMakeLists.txt.
    #
    # Code signing & notarization are intentionally NOT wired here:
    # the CPACK_BUNDLE_* variables (including APPLE_CERT_APP) apply
    # only to the Bundle generator, not DragNDrop. When signing lands
    # it belongs in CI: codesign the .app before cpack, then
    # `notarytool submit` + `stapler staple` on the finished DMG.

# ============================================================================
# Windows — NSIS installer
# ============================================================================
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "GitBolt")
    set(CPACK_NSIS_PACKAGE_NAME "GitBolt")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\GitBolt.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/ThePieMonster/GitBolt")
    set(CPACK_NSIS_HELP_LINK "https://github.com/ThePieMonster/GitBolt/issues")
    set(CPACK_NSIS_MODIFY_PATH ON)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    # Start menu shortcuts
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\GitBolt.lnk' '$INSTDIR\\\\bin\\\\GitBolt.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$START_MENU\\\\GitBolt.lnk'")

    # Explorer context menu integration via registry
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt' '' 'Open in GitBolt'
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt' 'Icon' '$INSTDIR\\\\bin\\\\GitBolt.exe'
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt\\\\command' '' '$INSTDIR\\\\bin\\\\GitBolt.exe \\\"%V\\\"'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt' '' 'Open in GitBolt'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt' 'Icon' '$INSTDIR\\\\bin\\\\GitBolt.exe'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt\\\\command' '' '$INSTDIR\\\\bin\\\\GitBolt.exe \\\"%V\\\"'
    ")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
        DeleteRegKey HKCR 'Directory\\\\shell\\\\gitbolt'
        DeleteRegKey HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt'
    ")

# ============================================================================
# Linux — DEB, AppImage, and TGZ
# ============================================================================
else()
    set(CPACK_GENERATOR "DEB;TGZ")

    # DEB package settings
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "GitBolt Team <team@gitbolt.dev>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # Let dpkg-shlibdeps compute the real shared-library dependencies
    # (libqt6svg6, the actual libgit2 soname of the build host, etc.)
    # instead of hand-pinning a list that drifts — the old hardcoded
    # "libgit2-1.7" pin was already wrong on newer Ubuntu, and it
    # omitted Qt SVG entirely.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/ThePieMonster/GitBolt")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

    # Install Linux desktop integration files
    install(FILES "${CMAKE_SOURCE_DIR}/src/app/platform/linux/gitbolt.desktop"
            DESTINATION share/applications)
    install(FILES "${CMAKE_SOURCE_DIR}/src/app/platform/linux/gitbolt.appdata.xml"
            DESTINATION share/metainfo)
    install(FILES "${CMAKE_SOURCE_DIR}/src/app/platform/linux/nautilus-gitbolt.py"
            DESTINATION share/nautilus-python/extensions
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

    # Install icon PNGs into the freedesktop hicolor theme so the
    # .desktop file's "Icon=gitbolt" reference resolves at runtime.
    # Each source PNG is pulled directly from the canonical
    # resources/icons/ folder, so icon regeneration (e.g. via
    # tools/generate-icon.py) automatically propagates to the next
    # packaged DEB / TGZ with no further wiring.
    foreach(_size 16 32 48 64 128 256 512)
        install(FILES "${CMAKE_SOURCE_DIR}/resources/icons/gitbolt-${_size}.png"
                DESTINATION "share/icons/hicolor/${_size}x${_size}/apps"
                RENAME gitbolt.png)
    endforeach()

    # AppImage support (used via linuxdeploy in CI, not CPack directly)
    # The CI workflow handles AppImage creation with linuxdeploy
endif()

# ============================================================================
# Common output settings
# ============================================================================
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES ON)
set(CPACK_SOURCE_IGNORE_FILES "/\\\\.git/;/build/;/\\\\.cache/")

include(CPack)
