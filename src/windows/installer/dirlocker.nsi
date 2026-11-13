Unicode True

!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef APP_VERSION
  !define APP_VERSION "2.2.1"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "DirLocker_Setup_v2.2.1.exe"
!endif
!ifndef SRC_WIN_DIR
  !define SRC_WIN_DIR ".."
!endif
!ifndef BIN_DIR
  !define BIN_DIR "../../../build-win64"
!endif

Name "目录密匣 v${APP_VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\DirLocker"
InstallDirRegKey HKLM "Software\DirLocker" "InstallDir"
RequestExecutionLevel admin
SetCompressor zlib
ShowInstDetails show
ShowUnInstDetails show

!define MUI_ICON "${SRC_WIN_DIR}\app.ico"
!define MUI_UNICON "${SRC_WIN_DIR}\app.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION "LaunchUnelevatedApp"
!define MUI_FINISHPAGE_RUN_TEXT "立即启动 目录密匣"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "ProductName" "目录密匣 (DirLocker)"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "CompanyName" "yinchaow & Gemini"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "FileDescription" "目录密匣 安装程序"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "FileVersion" "${APP_VERSION}.0"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "ProductVersion" "${APP_VERSION}.0"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "LegalCopyright" "Copyright (C) 2026 yinchaow & Gemini"

Function .onInit
  SetRegView 64
  SetShellVarContext all
  ReadRegStr $0 HKLM "Software\DirLocker" "InstallDir"
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function LaunchUnelevatedApp
  ; Launch via Windows shell explorer.exe so main window and drag-and-drop run at standard user integrity
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\dirlocker.exe"'
FunctionEnd

!macro StopAndRemoveOldExe EXE_PATH TAG
  ${If} ${FileExists} "${EXE_PATH}"
    ClearErrors
    GetDLLVersion "${EXE_PATH}" $R0 $R1
    ${IfNot} ${Errors}
      ; --quit-silent was introduced in v1.4.0.0 (0x00010004 = 65540).
      ; Never run --quit-silent on v1.2.x or older executables, or they will open their main window!
      ${If} $R0 >= 65540
        ExecWait '"${EXE_PATH}" --quit-silent'
        Sleep 700
      ${EndIf}
    ${EndIf}
    retry_del_${TAG}:
    ClearErrors
    Delete "${EXE_PATH}"
    ${If} ${Errors}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "检测到旧版 目录密匣 正在后台运行，请先右键系统托盘图标选择「退出」，再点击「重试」继续安装。" IDRETRY retry_del_${TAG}
      Abort
    ${EndIf}
  ${EndIf}
!macroend

Section "主程序与加密目录关联 (必需)" SecCore
  SectionIn RO

  ; Clean up legacy per-user v1.x install in %LOCALAPPDATA%\Programs\DirLocker if present
  SetShellVarContext current
  !insertmacro StopAndRemoveOldExe "$LOCALAPPDATA\Programs\DirLocker\dirlocker.exe" "legacy"
  Delete "$LOCALAPPDATA\Programs\DirLocker\Uninstall.exe"
  RMDir "$LOCALAPPDATA\Programs\DirLocker"
  Delete "$DESKTOP\目录密匣.lnk"
  Delete "$SMPROGRAMS\DirLocker 目录锁.lnk"
  Delete "$SMPROGRAMS\目录密盾.lnk"
  Delete "$SMPROGRAMS\目录密匣.lnk"
  Delete "$SMPROGRAMS\目录密匣\目录密匣.lnk"
  Delete "$SMPROGRAMS\目录密匣\卸载 目录密匣.lnk"
  RMDir "$SMPROGRAMS\目录密匣"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "DirLocker"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker"
  DeleteRegKey HKCU "Software\DirLocker"

  SetShellVarContext all
  !insertmacro StopAndRemoveOldExe "$INSTDIR\dirlocker.exe" "inst"

  SetOutPath "$INSTDIR"
  File "${BIN_DIR}\dirlocker.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ExecWait '"$INSTDIR\dirlocker.exe" --register-no-autostart'

  WriteRegStr HKLM "Software\DirLocker" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "DisplayName" "目录密匣"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "Publisher" "yinchaow & Gemini"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "DisplayIcon" "$INSTDIR\dirlocker.exe,0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker" "NoRepair" 1
SectionEnd

Section "开机自动启动（后台托盘保护与自动回锁）" SecAutoStart
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "DirLocker" '"$INSTDIR\dirlocker.exe" --tray'
SectionEnd

Section "开始菜单快捷方式" SecStartMenu
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\目录密匣"
  CreateShortcut "$SMPROGRAMS\目录密匣\目录密匣.lnk" "$INSTDIR\dirlocker.exe" "--show" "$INSTDIR\dirlocker.exe" 0
  CreateShortcut "$SMPROGRAMS\目录密匣\卸载 目录密匣.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0
SectionEnd

Section "桌面快捷方式" SecDesktop
  SetShellVarContext all
  CreateShortcut "$DESKTOP\目录密匣.lnk" "$INSTDIR\dirlocker.exe" "--show" "$INSTDIR\dirlocker.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "安装目录密匣核心程序至 Program Files，并注册右键菜单与无角标加密目录图标。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutoStart} "登录 Windows 时静默启动后台托盘保护，支持双击加密目录直接弹窗及闲置自动回锁。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "在开始菜单中创建“目录密匣”启动与卸载快捷方式。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "在桌面创建“目录密匣”快捷方式。"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ${If} ${FileExists} "$INSTDIR\dirlocker.exe"
    ExecWait '"$INSTDIR\dirlocker.exe" --uninstall-confirm' $0
    ${If} $0 != 0
      Abort
    ${EndIf}
    ClearErrors
    GetDLLVersion "$INSTDIR\dirlocker.exe" $R0 $R1
    ${IfNot} ${Errors}
      ${If} $R0 >= 65540
        ExecWait '"$INSTDIR\dirlocker.exe" --quit-silent'
        Sleep 700
      ${EndIf}
    ${EndIf}
    ExecWait '"$INSTDIR\dirlocker.exe" --unregister'
  ${EndIf}

  SetShellVarContext all
  Delete "$DESKTOP\目录密匣.lnk"
  Delete "$SMPROGRAMS\目录密匣\目录密匣.lnk"
  Delete "$SMPROGRAMS\目录密匣\卸载 目录密匣.lnk"
  RMDir "$SMPROGRAMS\目录密匣"

  SetShellVarContext current
  Delete "$DESKTOP\目录密匣.lnk"
  Delete "$SMPROGRAMS\DirLocker 目录锁.lnk"
  Delete "$SMPROGRAMS\目录密盾.lnk"
  Delete "$SMPROGRAMS\目录密匣.lnk"
  Delete "$SMPROGRAMS\目录密匣\目录密匣.lnk"
  Delete "$SMPROGRAMS\目录密匣\卸载 目录密匣.lnk"
  RMDir "$SMPROGRAMS\目录密匣"

  Delete "$INSTDIR\dirlocker.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "DirLocker"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "DirLocker"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\DirLocker"
  DeleteRegKey HKLM "Software\DirLocker"
  DeleteRegKey HKCU "Software\DirLocker"
SectionEnd
