# ============================================================================
# GitBolt CPack Packaging Configuration
# Supports: DMG (macOS), NSIS (Windows), DEB (Linux), and the symbol
# files of each (gitbolt_symbols, below)
# ============================================================================

set(CPACK_PACKAGE_NAME "GitBolt")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "GitBolt")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast cross-platform Git GUI client")
set(CPACK_PACKAGE_DESCRIPTION "GitBolt is a high-performance Git GUI client built with Qt 6 and libgit2.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://www.gitbolt.com")
set(CPACK_PACKAGE_CONTACT "GitBolt Team <support@gitbolt.com>")
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
    # license ships inside the bundle instead (Contents/Resources/LICENSE
    # and THIRD-PARTY-NOTICES.txt, see src/app/CMakeLists.txt), and as an
    # agreement step only where one is normal (NSIS).
    set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
    # The DragNDrop generator adds the /Applications symlink itself, so
    # the bundle sits at the DMG root with no install-prefix override.
    # Qt is deployed into the bundle during CPack's staging install via
    # the qt_generate_deploy_app_script hook in src/app/CMakeLists.txt.
    #
    # The bundle is signed by src/app/platform/macos/FinalizeBundle.cmake
    # (GitBolt's certificate in CI, else ad hoc), an install(SCRIPT) rule
    # after the Qt deploy step in src/app/CMakeLists.txt: CPack re-stages
    # the .app itself, so signing happens at install time. Notarization is
    # not wired: it needs an Apple-issued Developer ID, -o runtime
    # --timestamp, then notarytool and stapler on the finished DMG.

# ============================================================================
# Windows — NSIS installer
# ============================================================================
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "GitBolt")
    set(CPACK_NSIS_PACKAGE_NAME "GitBolt")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\GitBolt.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://www.gitbolt.com")
    set(CPACK_NSIS_HELP_LINK "https://github.com/Inverian/GitBolt/issues")
    # Not ON: bin\ holds Qt6*.dll and the MSVC runtime (plus vcpkg's
    # git2/pcre/z DLLs in a build against vcpkg's libgit2), which on the
    # system PATH could shadow other programs' DLLs, and a GUI app gains
    # nothing from being on PATH.
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    # The installer's own version resource, which Explorer's Properties >
    # Details shows (GitBolt.exe's is in src/app/platform/windows/
    # gitbolt.rc.in). CPack puts CPACK_NSIS_DEFINES near the top of its
    # NSIS template, and sets it only for component installs, which
    # GitBolt's isn't; CI's NSIS smoke test reads the version back.
    # GitBoltMinimumWindows: the oldest Windows this release runs on, which
    # the updater reads before installing it (src/update/Installer.h's
    # kWindowsMinimumKey).
    set(CPACK_NSIS_DEFINES "
  VIProductVersion '${PROJECT_VERSION}.0'
  VIFileVersion '${PROJECT_VERSION}.0'
  VIAddVersionKey 'ProductName' 'GitBolt'
  VIAddVersionKey 'CompanyName' 'GitBolt'
  VIAddVersionKey 'FileDescription' 'GitBolt Setup'
  VIAddVersionKey 'FileVersion' '${PROJECT_VERSION}'
  VIAddVersionKey 'ProductVersion' '${PROJECT_VERSION}'
  VIAddVersionKey 'GitBoltMinimumWindows' '${GITBOLT_WINDOWS_MINIMUM}'
")

    # Ship the MSVC runtime (msvcp140*.dll, vcruntime140*.dll,
    # concrt140.dll) app-local in bin\ from VS's redist folder. CI runners
    # have the VC++ redist in System32, so without this the installer
    # passes in CI but fails on a clean Windows ("VCRUNTIME140_1.dll was
    # not found"), and an older system msvcp140.dll hits the VS 17.10+
    # std::mutex crash. The UCRT is part of Windows 10+.
    include(InstallRequiredSystemLibraries)

    # The licence texts (cmake/Notices.cmake) in $INSTDIR, next to bin\;
    # the installer's licence page shows LICENSE but leaves no copy.
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt)
    install(FILES "${GITBOLT_NOTICES_FILE}" DESTINATION .)

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
# Linux — DEB and TGZ
# ============================================================================
else()
    set(CPACK_GENERATOR "DEB;TGZ")

    # DEB package settings
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "GitBolt Team <support@gitbolt.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # Let dpkg-shlibdeps compute the real shared-library dependencies:
    # the system libraries the BUNDLED Qt needs (xcb-cursor,
    # xkbcommon-x11, fontconfig, GL, ...), instead of hand-pinning a list
    # that drifts. The bundled Qt itself (/usr/lib/gitbolt, see
    # src/app/CMakeLists.txt) has no package. Released DEBs link libgit2
    # statically (GITBOLT_BUNDLED_LIBGIT2, which CI checks); one built
    # against a system libgit2 would also depend on its soname's package,
    # which only that distribution release has.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # Plus OpenSSL 3, which shlibdeps can't see: the bundled Qt's TLS
    # backend (plugins/tls/libqopensslbackend.so) dlopen()s libssl at
    # runtime, so nothing links it. Without it every HTTPS request —
    # usage statistics (PRIVACY.md) — fails silently. CPack merges this
    # with the shlibdeps result. libssl3t64 is the package's name since
    # the 64-bit time_t transition (Ubuntu 24.04), libssl3 before it.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl3t64 | libssl3")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://www.gitbolt.com")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    # Nautilus's Open in GitBolt (share/nautilus-python, below) needs it;
    # apt installs Recommends by default, in-app updates included.
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "python3-nautilus")

    # The licence texts in one file, where Debian keeps a package's
    # (cmake/Notices.cmake).
    install(FILES "${GITBOLT_COPYRIGHT_FILE}" DESTINATION share/doc/gitbolt)

    # Install Linux desktop integration files
    install(FILES "${CMAKE_SOURCE_DIR}/src/app/platform/linux/gitbolt.desktop"
            DESTINATION share/applications)
    # Named after the AppStream <id>; validators reject a metainfo file
    # whose name doesn't match it (metainfo-filename-cid-mismatch).
    install(FILES "${CMAKE_SOURCE_DIR}/src/app/platform/linux/com.gitbolt.app.metainfo.xml"
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
endif()

# ============================================================================
# Common output settings
# ============================================================================
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES ON)
set(CPACK_SOURCE_IGNORE_FILES "/\\\\.git/;/build/;/\\\\.cache/")

# ============================================================================
# Symbol files (GITBOLT_RELEASE_SYMBOLS, cmake/Symbols.cmake): the
# gitbolt_symbols target writes <build>/${CPACK_PACKAGE_FILE_NAME}-symbols.zip,
# which CI publishes beside the installers. It reads the build tree's
# binary, the one CPack installs (and strips), so the zip matches it.
# ============================================================================
if(GITBOLT_RELEASE_SYMBOLS)
    if(APPLE)
        # macdeployqt strips only the installed copy; the UUID survives
        # that and signing.
        find_program(GITBOLT_DSYMUTIL dsymutil REQUIRED)
        set(_gitbolt_split COMMAND ${GITBOLT_DSYMUTIL} $<TARGET_FILE:gitbolt> -o symbols/GitBolt.dSYM)
        set(_gitbolt_symbol_files GitBolt.dSYM)
    elseif(WIN32)
        # llvm-symbolizer wants the .exe beside its .pdb.
        set(_gitbolt_split COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:gitbolt> $<TARGET_PDB_FILE:gitbolt> symbols/)
        set(_gitbolt_symbol_files $<TARGET_FILE_NAME:gitbolt> $<TARGET_PDB_FILE_NAME:gitbolt>)
    else()
        # The binary gets a debug link to gitbolt.debug, which CPack's
        # strip keeps; the old link goes first, so a rerun links the new file.
        set(_gitbolt_split
            COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:gitbolt> symbols/gitbolt.debug
            COMMAND ${CMAKE_OBJCOPY} --remove-section=.gnu_debuglink
                --add-gnu-debuglink=symbols/gitbolt.debug $<TARGET_FILE:gitbolt>)
        set(_gitbolt_symbol_files gitbolt.debug)
    endif()
    add_custom_target(gitbolt_symbols
        COMMAND ${CMAKE_COMMAND} -E rm -rf symbols
        COMMAND ${CMAKE_COMMAND} -E make_directory symbols
        ${_gitbolt_split}
        COMMAND ${CMAKE_COMMAND} -E chdir symbols ${CMAKE_COMMAND} -E tar cf
            "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}-symbols.zip" --format=zip --
            ${_gitbolt_symbol_files}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Symbol files: ${CPACK_PACKAGE_FILE_NAME}-symbols.zip"
        VERBATIM)
    add_dependencies(gitbolt_symbols gitbolt)
endif()

include(CPack)
