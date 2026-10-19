Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "nsDialogs.nsh"
!include "FileFunc.nsh"

!ifndef APP_VERSION
  !error "APP_VERSION is required"
!endif
!ifndef PAYLOAD_DIR
  !error "PAYLOAD_DIR is required"
!endif
!ifndef SETUP_FILE
  !error "SETUP_FILE is required"
!endif

!define PRODUCT_ID "Kique261.PartyMic"
!define APP_KEY "Software\Kique261\PartyMic"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\PartyMic"
!define CABLE_URL "https://vb-audio.com/Cable/"

Name "PartyMic ${APP_VERSION}"
OutFile "${SETUP_FILE}"
InstallDir "$LOCALAPPDATA\Programs\PartyMic"
RequestExecutionLevel user
SetCompressor /SOLID zlib
SetOverwrite on
ShowInstDetails show
ShowUninstDetails show
BrandingText "PartyMic · 开黑背景音乐"
Icon "../src/windows/assets/partymic.ico"
UninstallIcon "../src/windows/assets/partymic.ico"
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=2052 "ProductName" "PartyMic"
VIAddVersionKey /LANG=2052 "FileDescription" "PartyMic 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "PartyMic contributors"
ManifestDPIAwareness PerMonitorV2

Var DriverDialog
Var DriverLabel
Var DriverLink
Var DriverReady
Var DesktopCheck
Var DesktopEnabled
Var ExistingDir
Var SetupMutex

!define MUI_ABORTWARNING
!define MUI_ICON "../src/windows/assets/partymic.ico"
!define MUI_UNICON "../src/windows/assets/partymic.ico"
!define MUI_WELCOMEPAGE_TITLE "安装 PartyMic"
!define MUI_WELCOMEPAGE_TEXT "把音乐带进每一次开黑。$\r$\n$\r$\n此向导将为当前 Windows 账户安装 PartyMic，并创建启动和卸载入口。$\r$\n$\r$\n混音需要 VB-Audio 官方 VB-CABLE 驱动。下一页提供官网入口；本安装包不包含或自动安装驱动。$\r$\n$\r$\n安装前请先退出正在运行的 PartyMic。"
!insertmacro MUI_PAGE_WELCOME
Page custom DriverPage
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirectoryLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "PartyMic 已安装"
!define MUI_FINISHPAGE_TEXT "可以从开始菜单打开 PartyMic。$\r$\n$\r$\n若尚未安装 VB-CABLE，请从官网单独安装并重启 Windows。然后在 PartyMic 选择 CABLE Input，在语音软件选择 CABLE Output。"
!define MUI_FINISHPAGE_RUN "$INSTDIR\PartyMic.exe"
!define MUI_FINISHPAGE_RUN_TEXT "启动 PartyMic"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
  SetShellVarContext current
  SetRegView 64
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\PartyMicSetup") p .r0 ?e'
  Pop $1
  StrCpy $SetupMutex $0
  ${If} $1 = 183
    IfSilent +2
    MessageBox MB_OK|MB_ICONINFORMATION "另一个 PartyMic 安装程序正在运行。"
    SetErrorLevel 3
    Abort
  ${EndIf}
  ${IfNot} ${IsNativeAMD64}
    IfSilent +2
    MessageBox MB_OK|MB_ICONSTOP "PartyMic 需要 Windows 11 x64。"
    SetErrorLevel 4
    Abort
  ${EndIf}
  ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion" "CurrentBuildNumber"
  ${If} $0 < 22000
    IfSilent +2
    MessageBox MB_OK|MB_ICONSTOP "PartyMic 需要 Windows 11 或更新版本。"
    SetErrorLevel 4
    Abort
  ${EndIf}
  ReadRegStr $ExistingDir HKCU "${APP_KEY}" "InstallDir"
  ${If} $ExistingDir != ""
    StrCpy $INSTDIR $ExistingDir
  ${EndIf}
  StrCpy $DesktopEnabled ${BST_CHECKED}
FunctionEnd

; Read-only standard stereo endpoint detection; no default/device changes.
Function DetectCable
  StrCpy $DriverReady 0
  StrCpy $0 0
  render_loop:
    ClearErrors
    EnumRegKey $1 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render" $0
    IfErrors render_done
    StrCmp $1 "" render_done
    IntOp $0 $0 + 1
    ReadRegDWORD $2 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$1" "DeviceState"
    IntOp $2 $2 & 1
    ${If} $2 = 1
      ReadRegStr $2 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$1\Properties" "{a45c254e-df1c-4efd-8020-67d146a850e0},2"
      ReadRegStr $3 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$1\Properties" "{b3f8fa53-0004-438e-9003-51a46e139bfc},6"
      ${If} $2 == "CABLE Input"
      ${AndIf} $3 == "VB-Audio Virtual Cable"
        StrCpy $DriverReady 1
        Goto render_done
      ${EndIf}
    ${EndIf}
    Goto render_loop
  render_done:
  ${If} $DriverReady != 1
    Return
  ${EndIf}
  StrCpy $DriverReady 0
  StrCpy $0 0
  capture_loop:
    ClearErrors
    EnumRegKey $1 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture" $0
    IfErrors capture_done
    StrCmp $1 "" capture_done
    IntOp $0 $0 + 1
    ReadRegDWORD $2 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$1" "DeviceState"
    IntOp $2 $2 & 1
    ${If} $2 = 1
      ReadRegStr $2 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$1\Properties" "{a45c254e-df1c-4efd-8020-67d146a850e0},2"
      ReadRegStr $3 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$1\Properties" "{b3f8fa53-0004-438e-9003-51a46e139bfc},6"
      ${If} $2 == "CABLE Output"
      ${AndIf} $3 == "VB-Audio Virtual Cable"
        StrCpy $DriverReady 1
        Goto capture_done
      ${EndIf}
    ${EndIf}
    Goto capture_loop
  capture_done:
FunctionEnd

Function DriverPage
  !insertmacro MUI_HEADER_TEXT "虚拟声卡与快捷方式" "官方驱动独立安装，PartyMic 使用当前账户安装。"
  nsDialogs::Create 1018
  Pop $DriverDialog
  ${If} $DriverDialog == error
    Abort
  ${EndIf}
  Call DetectCable
  ${If} $DriverReady = 1
    ${NSD_CreateLabel} 0 0 100% 24u "已检测到标准 CABLE Input / CABLE Output。可继续安装 PartyMic。"
  ${Else}
    ${NSD_CreateLabel} 0 0 100% 24u "未检测到已启用的标准 VB-CABLE 线路。可以先安装 PartyMic，再安装官方驱动。"
  ${EndIf}
  Pop $DriverLabel
  ${NSD_CreateLabel} 0 28u 100% 43u "首次使用：打开 VB-Audio 官网，下载 Windows 驱动包，解压后以管理员身份运行 VBCABLE_Setup_x64.exe，完成后重启。$\r$\n$\r$\nVB-CABLE 为 VB-Audio 的 donationware，使用遵循其官方许可。"
  Pop $DriverLabel
  ${NSD_CreateButton} 0 77u 155u 20u "打开 VB-CABLE 官方网站"
  Pop $DriverLink
  ${NSD_OnClick} $DriverLink OpenCableSite
  ${NSD_CreateLabel} 0 102u 100% 12u "本向导只打开官网，不下载、不捆绑或自动安装驱动。"
  Pop $DriverLabel
  ${NSD_CreateCheckbox} 0 121u 100% 14u "创建桌面快捷方式"
  Pop $DesktopCheck
  ${NSD_SetState} $DesktopCheck $DesktopEnabled
  ${NSD_OnClick} $DesktopCheck DesktopChanged
  nsDialogs::Show
FunctionEnd

Function OpenCableSite
  ExecShell "open" "${CABLE_URL}"
FunctionEnd
Function DesktopChanged
  ${NSD_GetState} $DesktopCheck $DesktopEnabled
FunctionEnd

; Refuse overwriting arbitrary nonempty folders. Our marker allows upgrades.
Function DirectoryLeave
  ; NSIS GetFullPathName fails when a parent folder does not exist yet.
  ; The Win32 API canonicalizes new paths without creating them.
  System::Call 'kernel32::GetFullPathNameW(w "$INSTDIR", i ${NSIS_MAX_STRLEN}, w .r0, p 0) i .r1'
  ${If} $1 = 0
  ${OrIf} $1 >= ${NSIS_MAX_STRLEN}
    IfSilent +2
    MessageBox MB_OK|MB_ICONEXCLAMATION "安装路径无效或过长，请选择其他文件夹。"
    SetErrorLevel 5
    Abort
  ${EndIf}
  StrCpy $INSTDIR $0
  ${GetRoot} "$INSTDIR" $0
  ${If} $INSTDIR == $0
  ${OrIf} $INSTDIR == "$0\"
    IfSilent +2
    MessageBox MB_OK|MB_ICONEXCLAMATION "请选择单独的 PartyMic 文件夹，不能直接安装到磁盘根目录。"
    SetErrorLevel 5
    Abort
  ${EndIf}
  ${If} $ExistingDir != ""
  ${AndIf} $ExistingDir != $INSTDIR
    IfSilent +2
    MessageBox MB_OK|MB_ICONEXCLAMATION "升级时请使用原安装目录；如需更换位置，请先卸载 PartyMic。"
    SetErrorLevel 5
    Abort
  ${EndIf}
  ReadINIStr $0 "$INSTDIR\PartyMic-install.ini" "Product" "Id"
  ${If} $0 == "${PRODUCT_ID}"
    Return
  ${EndIf}
  ClearErrors
  FindFirst $0 $1 "$INSTDIR\*"
  IfErrors empty_directory
  ${DoWhile} $1 != ""
    ${If} $1 != "."
    ${AndIf} $1 != ".."
      FindClose $0
      IfSilent +2
      MessageBox MB_OK|MB_ICONEXCLAMATION "请选择空文件夹，或此前由本安装程序创建的 PartyMic 安装目录。"
      SetErrorLevel 5
      Abort
    ${EndIf}
    FindNext $0 $1
    IfErrors directory_enumerated
  ${Loop}
  directory_enumerated:
  FindClose $0
  empty_directory:
FunctionEnd

!macro RequireClosed Prefix
Function ${Prefix}RequireClosed
  FindWindow $0 "PartyMicWindow"
  ${If} $0 != 0
    IfSilent +2
    MessageBox MB_OK|MB_ICONEXCLAMATION "请先关闭 PartyMic，再安装或卸载。录音将按程序正常流程保存。"
    SetErrorLevel 2
    Abort
  ${EndIf}
FunctionEnd
!macroend
!insertmacro RequireClosed ""
!insertmacro RequireClosed "un."

Section "PartyMic" MainSection
  Call DirectoryLeave
  Call RequireClosed
  SetOutPath "$INSTDIR"
  ClearErrors
  File "${PAYLOAD_DIR}/PartyMic.exe"
  File "${PAYLOAD_DIR}/README.md"
  File "${PAYLOAD_DIR}/THIRD_PARTY_NOTICES.txt"
  SetOutPath "$INSTDIR\docs"
  File "${PAYLOAD_DIR}/docs/install.md"
  File "${PAYLOAD_DIR}/docs/usage.md"
  File "${PAYLOAD_DIR}/docs/troubleshooting.md"
  SetOutPath "$INSTDIR\docs\assets"
  File "${PAYLOAD_DIR}/docs/assets/partymic-mixer.png"
  SetOutPath "$INSTDIR\licenses"
  File "${PAYLOAD_DIR}/licenses/*"
  SetOutPath "$INSTDIR"
  ${If} ${Errors}
    SetErrorLevel 6
    Abort "无法写入安装文件。请关闭 PartyMic 并检查目录权限。"
  ${EndIf}
  WriteINIStr "$INSTDIR\PartyMic-install.ini" "Product" "Id" "${PRODUCT_ID}"
  WriteINIStr "$INSTDIR\PartyMic-install.ini" "Product" "Version" "${APP_VERSION}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\PartyMic"
  CreateShortcut "$SMPROGRAMS\PartyMic\PartyMic.lnk" "$INSTDIR\PartyMic.exe" "" "$INSTDIR\PartyMic.exe" 0
  CreateShortcut "$SMPROGRAMS\PartyMic\卸载 PartyMic.lnk" "$INSTDIR\Uninstall.exe"
  ${If} $DesktopEnabled = ${BST_CHECKED}
    CreateShortcut "$DESKTOP\PartyMic.lnk" "$INSTDIR\PartyMic.exe" "" "$INSTDIR\PartyMic.exe" 0
  ${EndIf}
  WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "PartyMic"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Kique261"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\PartyMic.exe,0"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/Kique261/PartyMic"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" 1500
SectionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\PartyMicSetup") p .r0 ?e'
  Pop $1
  StrCpy $SetupMutex $0
  ${If} $1 = 183
    IfSilent +2
    MessageBox MB_OK|MB_ICONINFORMATION "另一个 PartyMic 安装或卸载程序正在运行。"
    SetErrorLevel 3
    Abort
  ${EndIf}
  ReadINIStr $0 "$INSTDIR\PartyMic-install.ini" "Product" "Id"
  ${If} $0 != "${PRODUCT_ID}"
    SetErrorLevel 5
    Abort "此目录不是本安装程序创建的 PartyMic 安装目录。"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  Call un.RequireClosed
  Delete "$INSTDIR\PartyMic.exe"
  ${If} ${FileExists} "$INSTDIR\PartyMic.exe"
    SetErrorLevel 6
    Abort "无法移除程序。请关闭 PartyMic 后重试。"
  ${EndIf}
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
  Delete "$INSTDIR\docs\install.md"
  Delete "$INSTDIR\docs\usage.md"
  Delete "$INSTDIR\docs\troubleshooting.md"
  Delete "$INSTDIR\docs\assets\partymic-mixer.png"
  RMDir "$INSTDIR\docs\assets"
  RMDir "$INSTDIR\docs"
  Delete "$INSTDIR\licenses\LLVM-LICENSE.txt"
  Delete "$INSTDIR\licenses\COPYING.MinGW-w64-runtime.txt"
  Delete "$INSTDIR\licenses\COPYING.winpthreads.txt"
  Delete "$INSTDIR\licenses\NSIS-LICENSE.txt"
  RMDir "$INSTDIR\licenses"
  Delete "$INSTDIR\PartyMic-install.ini"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$SMPROGRAMS\PartyMic\PartyMic.lnk"
  Delete "$SMPROGRAMS\PartyMic\卸载 PartyMic.lnk"
  RMDir "$SMPROGRAMS\PartyMic"
  Delete "$DESKTOP\PartyMic.lnk"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"
  ; No recursive directory delete: preserve recordings and unowned files.
  RMDir "$INSTDIR"
SectionEnd
