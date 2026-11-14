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
# The installer's exit code when a file of the install is in use and it
# changed nothing: the updater's kWindowsInstallerExitInUse
# (src/update/InstallHelpers.h). NSIS's own are 1 (cancelled), 2 (aborted).
set(_gitbolt_nsis_in_use_exit 3)

# Before any Section (CPack overwrites it only for component installs).
# The installer's version resource, for Explorer and CI's smoke test;
# GitBoltMinimumWindows, which the updater reads first (Installer.h's
# kWindowsMinimumKey). AllowSkipFiles off: a file that comes into use
# after the check below fails the install (exit code 2 under /S, where
# the box's default is Cancel) instead of being skipped while it succeeds.
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

# Uninstall-before-install, replacing CPack's .onInit one (its "Uninstall
# failed." has no /SD). First GitBolt's files (the folder's own, all of bin
# and plugins) in the registered folder and $INSTDIR, opened as File opens
# them: if one is in use, nothing changes (Retry/Cancel; /S: the in-use exit
# code), not the old uninstaller removing the rest and File stopping on it.
# Then that uninstaller from a copy; the new one registered before any file.
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
        StrCmp $1 '\\\\${_gitbolt_nsis_uninstaller}' 0 gitbolt_unregistered
        IfFileExists $0 0 gitbolt_unregistered
        StrCpy $1 $0 -${_gitbolt_nsis_uninstaller_length}
        Goto gitbolt_check_in_use
      gitbolt_unregistered:
        StrCpy $0 ''
        StrCpy $1 ''
      gitbolt_check_in_use:
        StrCpy $9 0
        StrCpy $5 $1
        StrCmp $5 '' gitbolt_check_instdir
      gitbolt_open_folder:
        ClearErrors
        FindFirst $3 $4 '$5\\\\*.*'
        IfErrors gitbolt_folder_done
      gitbolt_next_entry:
        StrCmp $4 '' gitbolt_folder_done
        StrCmp $4 '.' gitbolt_skip_entry
        StrCmp $4 '..' gitbolt_skip_entry
        StrCpy $6 '$5\\\\$4'
        IfFileExists '$6\\\\*.*' 0 gitbolt_probe
        IntCmp $9 0 0 gitbolt_open_subfolder gitbolt_open_subfolder
        StrCmp $4 'bin' gitbolt_open_subfolder
        StrCmp $4 'plugins' gitbolt_open_subfolder gitbolt_skip_entry
      gitbolt_open_subfolder:
        Push $3
        Push $5
        IntOp $9 $9 + 1
        StrCpy $5 $6
        Goto gitbolt_open_folder
      gitbolt_probe:
        StrCmp $6 $EXEPATH gitbolt_skip_entry
        System::Call 'kernel32::CreateFile(t r6, i 0x40000000, i 1, p 0, i 3, i 0, p 0) p .r7 ?e'
        Pop $8
        StrCmp $7 -1 gitbolt_not_opened
        System::Call 'kernel32::CloseHandle(p r7)'
        Goto gitbolt_skip_entry
      gitbolt_not_opened:
        StrCmp $8 32 gitbolt_in_use
        StrCmp $8 33 gitbolt_in_use
      gitbolt_skip_entry:
        FindNext $3 $4
        Goto gitbolt_next_entry
      gitbolt_folder_done:
        FindClose $3
        IntCmp $9 0 gitbolt_root_done
        Pop $5
        Pop $3
        IntOp $9 $9 - 1
        Goto gitbolt_skip_entry
      gitbolt_root_done:
        StrCmp $5 $INSTDIR gitbolt_free
      gitbolt_check_instdir:
        StrCmp $1 $INSTDIR gitbolt_free
        IfFileExists '$INSTDIR\\\\${_gitbolt_nsis_uninstaller}' gitbolt_walk_instdir
        IfFileExists '$INSTDIR\\\\bin\\\\GitBolt.exe' 0 gitbolt_free
      gitbolt_walk_instdir:
        StrCpy $5 $INSTDIR
        Goto gitbolt_open_folder
      gitbolt_in_use:
        FindClose $3
      gitbolt_forget_folders:
        IntCmp $9 0 gitbolt_ask
        Pop $5
        Pop $3
        FindClose $3
        IntOp $9 $9 - 1
        Goto gitbolt_forget_folders
      gitbolt_ask:
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION 'GitBolt is running, or another program is using $6. Close it, then click Retry.' /SD IDCANCEL IDRETRY gitbolt_check_in_use
        SetErrorLevel ${_gitbolt_nsis_in_use_exit}
        Abort 'GitBolt is running, or another program is using $6, so nothing was installed.'
      gitbolt_free:
        StrCmp $0 '' gitbolt_uninstalled
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
# C:\Program.exe first (CWE-428). WriteUninstaller deletes the file when
# another program has it open (a virus scan, say), yet the install goes on;
# and one deleted while held stays listed until let go. So it must open and
# be as long as a fresh copy, else it is written again for up to 28 s, then
# the install fails. Last, Installed apps' folder and size in KB.
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt' '' 'Open in GitBolt'
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt' 'Icon' '$INSTDIR\\\\bin\\\\GitBolt.exe'
        WriteRegStr HKCR 'Directory\\\\shell\\\\gitbolt\\\\command' '' '\\\"$INSTDIR\\\\bin\\\\GitBolt.exe\\\" \\\"%V\\\"'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt' '' 'Open in GitBolt'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt' 'Icon' '$INSTDIR\\\\bin\\\\GitBolt.exe'
        WriteRegStr HKCR 'Directory\\\\Background\\\\shell\\\\gitbolt\\\\command' '' '\\\"$INSTDIR\\\\bin\\\\GitBolt.exe\\\" \\\"%V\\\"'
        StrCpy $3 ''
        InitPluginsDir
        ClearErrors
        WriteUninstaller '$PLUGINSDIR\\\\gitbolt-uninstaller.exe'
        IfErrors gitbolt_reference_done
        FileOpen $0 '$PLUGINSDIR\\\\gitbolt-uninstaller.exe' r
        FileSeek $0 0 END $3
        FileClose $0
      gitbolt_reference_done:
        StrCpy $2 0
      gitbolt_check_uninstaller:
        ClearErrors
        FileOpen $0 '$INSTDIR\\\\${_gitbolt_nsis_uninstaller}' r
        IfErrors gitbolt_rewrite_uninstaller
        FileSeek $0 0 END $1
        FileClose $0
        StrCmp $3 '' gitbolt_have_uninstaller
        StrCmp $1 $3 gitbolt_have_uninstaller
      gitbolt_rewrite_uninstaller:
        IntOp $2 $2 + 1
        IntCmp $2 8 gitbolt_no_uninstaller 0 gitbolt_no_uninstaller
        DetailPrint 'Writing the uninstaller again'
        IntOp $4 $2 * 1000
        Sleep $4
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
# uninstaller reports success. Not when silent: the installer runs it so
# right after checking GitBolt's files (above); a script can't answer a box.
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
