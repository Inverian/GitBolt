# The NSIS script GitBolt adds to CPack's NSIS.template.in through its
# hooks (not a copy of the template). Included by cmake/Packaging.cmake;
# tests/cmake/TestNsisScript.cmake checks it on every OS.
# CMake reads each value twice (here, then from CPackConfig.cmake), so
# NSIS gets \\\\ as \, \\\" as " and \\\${ as ${.

# The updater (src/update/Package.cpp, InstallHelpers.cpp) and CI look
# for the uninstall key, ...\Uninstall\GitBolt (32-bit view: the stub is
# 32-bit), and for Uninstall.exe by these names.
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "GitBolt")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
set(_gitbolt_nsis_key
    "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\${CPACK_PACKAGE_INSTALL_REGISTRY_KEY}")
set(_gitbolt_nsis_uninstaller "${CPACK_NSIS_UNINSTALL_NAME}.exe")
string(LENGTH "\\${_gitbolt_nsis_uninstaller}" _gitbolt_nsis_uninstaller_length)

# Before any Section (CPack overwrites it only for component installs).
# The installer's version resource, for Explorer and CI's smoke test;
# GitBoltMinimumWindows, which the updater reads first (Installer.h's
# kWindowsMinimumKey). AllowSkipFiles off: a file in use fails the
# install (exit code 2 under /S, where the box's default is Cancel)
# instead of being skipped while the install reports success.
set(CPACK_NSIS_DEFINES "
  VIProductVersion '${PROJECT_VERSION}.0'
  VIFileVersion '${PROJECT_VERSION}.0'
  VIAddVersionKey 'ProductName' 'GitBolt'
  VIAddVersionKey 'CompanyName' 'GitBolt'
  VIAddVersionKey 'FileDescription' 'GitBolt Setup'
  VIAddVersionKey 'FileVersion' '${PROJECT_VERSION}'
  VIAddVersionKey 'ProductVersion' '${PROJECT_VERSION}'
  VIAddVersionKey 'GitBoltMinimumWindows' '${GITBOLT_WINDOWS_MINIMUM}'
  AllowSkipFiles off
  !include FileFunc.nsh
")

# Uninstall-before-install, replacing CPack's .onInit one: its "Uninstall
# failed." (no /SD) hangs a silent install once Uninstall.exe is lost. Runs
# the registered uninstaller only if it is there, from a copy (so the
# installed one is free to be rewritten), else installs over the old files.
# Then registers the new one before any file, so an install that stops
# halfway (a file in use) can still be repaired by an update, or removed.
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)
set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS "
        ReadRegStr $0 HKLM '${_gitbolt_nsis_key}' 'UninstallString'
        StrCpy $1 $0 1
        StrCmp $1 '\\\"' 0 +2
        StrCpy $0 $0 '' 1
        StrCpy $1 $0 1 -1
        StrCmp $1 '\\\"' 0 +2
        StrCpy $0 $0 -1
        StrCpy $1 $0 '' -${_gitbolt_nsis_uninstaller_length}
        StrCmp $1 '\\\\${_gitbolt_nsis_uninstaller}' 0 gitbolt_uninstalled
        IfFileExists $0 0 gitbolt_uninstalled
        StrCpy $1 $0 -${_gitbolt_nsis_uninstaller_length}
        DetailPrint 'Removing the installed GitBolt from $1'
        InitPluginsDir
        ClearErrors
        CopyFiles /SILENT $0 '$PLUGINSDIR\\\\gitbolt-old-uninstaller.exe'
        IfErrors gitbolt_uninstall_failed
        ExecWait '\\\"$PLUGINSDIR\\\\gitbolt-old-uninstaller.exe\\\" /S _?=$1' $2
        IfErrors gitbolt_uninstall_failed
        StrCmp $2 0 gitbolt_uninstalled
      gitbolt_uninstall_failed:
        DetailPrint 'The installed GitBolt could not be removed: installing over it'
      gitbolt_uninstalled:
        SetOutPath $INSTDIR
        WriteUninstaller '$INSTDIR\\\\${_gitbolt_nsis_uninstaller}'
        WriteRegStr SHCTX '${_gitbolt_nsis_key}' 'DisplayName' '${CPACK_NSIS_DISPLAY_NAME}'
        WriteRegStr SHCTX '${_gitbolt_nsis_key}' 'UninstallString' '\\\"$INSTDIR\\\\${_gitbolt_nsis_uninstaller}\\\"'
")

# Start menu shortcuts
set(CPACK_NSIS_CREATE_ICONS_EXTRA
    "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\GitBolt.lnk' '$INSTDIR\\\\bin\\\\GitBolt.exe'")
# CPack runs this after deleting Software\GitBolt\GitBolt, so the
# empty Software\GitBolt it leaves can go too.
set(CPACK_NSIS_DELETE_ICONS_EXTRA "
        Delete '$SMPROGRAMS\\\\$START_MENU\\\\GitBolt.lnk'
        DeleteRegKey /ifempty SHCTX 'Software\\\\GitBolt'
")

# Explorer's Open in GitBolt, the program quoted: unquoted, Windows tries
# C:\Program.exe first (CWE-428). WriteUninstaller deletes the file when it
# can't write it (in use) and the install still succeeds: it is tried
# again for a few seconds, else the install fails. Last, what Installed
# apps shows: the folder, and its size in KB.
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt' '' 'Open in GitBolt'
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt' 'Icon' '$INSTDIR\\\\bin\\\\GitBolt.exe'
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt\\\\command' '' '\\\"$INSTDIR\\\\bin\\\\GitBolt.exe\\\" \\\"%V\\\"'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt' '' 'Open in GitBolt'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt' 'Icon' '$INSTDIR\\\\bin\\\\GitBolt.exe'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt\\\\command' '' '\\\"$INSTDIR\\\\bin\\\\GitBolt.exe\\\" \\\"%V\\\"'
        StrCpy $0 0
      gitbolt_check_uninstaller:
        IfFileExists '$INSTDIR\\\\${_gitbolt_nsis_uninstaller}' gitbolt_have_uninstaller
        IntOp $0 $0 + 1
        IntCmp $0 5 gitbolt_no_uninstaller 0 gitbolt_no_uninstaller
        DetailPrint 'Writing the uninstaller again'
        Sleep 1000
        WriteUninstaller '$INSTDIR\\\\${_gitbolt_nsis_uninstaller}'
        Goto gitbolt_check_uninstaller
      gitbolt_no_uninstaller:
        Abort 'Could not create the uninstaller. Run the installer again.'
      gitbolt_have_uninstaller:
        WriteRegStr SHCTX '${_gitbolt_nsis_key}' 'InstallLocation' '$INSTDIR'
        ClearErrors
        \\\${GetSize} '$INSTDIR' '/S=0K' $0 $1 $2
        IfErrors gitbolt_size_unknown
        WriteRegDWORD SHCTX '${_gitbolt_nsis_key}' 'EstimatedSize' $0
      gitbolt_size_unknown:
")
# First, before anything is deleted: a running GitBolt.exe can't be
# opened for writing, and its files would be left behind while the
# uninstaller reports success. Not when silent: the update's
# installer runs it so, once its administrator part saw GitBolt quit.
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
        IfSilent gitbolt_not_running
        IfFileExists '$INSTDIR\\\\bin\\\\GitBolt.exe' 0 gitbolt_not_running
      gitbolt_check_running:
        ClearErrors
        FileOpen $0 '$INSTDIR\\\\bin\\\\GitBolt.exe' a
        IfErrors 0 gitbolt_closed
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION 'GitBolt is running. Quit GitBolt, then click Retry.' IDRETRY gitbolt_check_running
        Abort 'GitBolt was running, so nothing was removed.'
      gitbolt_closed:
        FileClose $0
      gitbolt_not_running:
        DeleteRegKey HKCR 'Directory\\\\shell\\\\gitbolt'
        DeleteRegKey HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt'
")
