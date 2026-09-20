; The Windows installer: for the current user, without administrator rights, into %LOCALAPPDATA%\Programs\Lexiglance.
; One window with a progress bar, then Lexiglance opens. Running a newer one updates in place; settings and dictionaries
; live elsewhere and stay. An update that fails part way puts the version before back. The uninstaller asks whether to
; delete settings, dictionaries and downloaded models too (yes unless unticked; /KEEPDATA keeps them when silent).
; Built from an install prefix by .github/scripts/windows-installer.ps1:
;   makensis /DVERSION=x.y.z /DSTAGE=<prefix> /DOUTFILE=<setup.exe> installer.nsi

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "nsDialogs.nsh"
!include "x64.nsh"

!ifndef VERSION
    !error "pass /DVERSION=x.y.z"
!endif
!ifndef STAGE
    !error "pass /DSTAGE=<install prefix>"
!endif
!ifndef OUTFILE
    !error "pass /DOUTFILE=<setup.exe>"
!endif

!define APP_KEY "Software\Lexiglance"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lexiglance"

Name "Lexiglance"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\Lexiglance"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ManifestDPIAware true
ShowInstDetails nevershow
ShowUninstDetails nevershow
AutoCloseWindow true
BrandingText "Lexiglance ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Lexiglance"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "Lexiglance installer"
VIAddVersionKey "LegalCopyright" "MattFor, MIT license"

!define MUI_ICON "lexiglance.ico"
!define MUI_UNICON "lexiglance.ico"
!insertmacro MUI_PAGE_INSTFILES
UninstPage custom un.ChoosePage un.ChooseLeave
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Whether the uninstaller leaves settings, dictionaries and downloaded models behind (1) or deletes them too (0).
Var KeepData
Var KeepBox

; The programs cannot be replaced or removed while they run: ended, and waited for (up to ten seconds) until Windows
; has let go of their files.
!macro STOP_RUNNING prefix
    Function ${prefix}StopRunning
        nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM lexiglance.exe'
        Pop $0
        nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM lexiglanced.exe'
        Pop $0
        nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM lexiglancectl.exe'
        Pop $0
        StrCpy $R9 0
        ${Do}
            ; find answers 0 while tasklist still lists the program, 1 once it is gone.
            nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$SYSDIR\tasklist.exe" /NH /FI "IMAGENAME eq lexiglanced.exe" | "$SYSDIR\find.exe" /I "lexiglanced.exe""'
            Pop $0
            nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$SYSDIR\tasklist.exe" /NH /FI "IMAGENAME eq lexiglance.exe" | "$SYSDIR\find.exe" /I "lexiglance.exe""'
            Pop $1
            ${If} $0 != 0
            ${AndIf} $1 != 0
                ${Break}
            ${EndIf}
            IntOp $R9 $R9 + 1
            ${If} $R9 >= 40
                ${Break}
            ${EndIf}
            Sleep 250
        ${Loop}
        Sleep 250
    FunctionEnd
!macroend
!insertmacro STOP_RUNNING ""
!insertmacro STOP_RUNNING "un."

; Reading text from the screen loads Microsoft's own onnxruntime.dll, which is built with MSVC and imports its
; redistributable; Lexiglance itself brings the UCRT along and needs none of it. Interactive installs ask; a silent
; self-update (/relaunch=...) installs it without asking so people coming from an older build get OCR working; a silent
; install with no relaunch (CI) leaves it alone.
Function OfferVcRedist
    ${If} ${IsNativeARM64}
        StrCpy $R1 "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\ARM64"
        StrCpy $R2 "https://aka.ms/vs/17/release/vc_redist.arm64.exe"
    ${Else}
        StrCpy $R1 "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64"
        StrCpy $R2 "https://aka.ms/vs/17/release/vc_redist.x64.exe"
    ${EndIf}
    SetRegView 64
    ReadRegDWORD $R0 HKLM $R1 "Installed"
    SetRegView lastused
    ${If} $R0 == 1
        Return
    ${EndIf}
    ${If} ${Silent}
        ${GetParameters} $0
        ClearErrors
        ${GetOptions} $0 "/relaunch=" $1
        ${If} ${Errors}
            Return
        ${EndIf}
    ${Else}
        MessageBox MB_YESNO|MB_ICONQUESTION "Reading text from the screen (OCR) needs the Microsoft Visual C++ Redistributable, which this computer does not have. Everything else works without it.$\n$\nDownload and install it now?" IDNO offered
    ${EndIf}
    StrCpy $R3 "$TEMP\lexiglance-vc-redist.exe"
    nsExec::Exec '"$SYSDIR\curl.exe" -sSL -o "$R3" "$R2"'
    Pop $R4
    ${If} $R4 == 0
    ${AndIf} ${FileExists} "$R3"
        ; It asks for administrator rights itself; /norestart so it never reboots behind the user's back.
        ExecWait '"$R3" /install /passive /norestart'
        Delete "$R3"
    ${Else}
        ${IfNot} ${Silent}
            ExecShell "open" "$R2"
        ${EndIf}
    ${EndIf}
    offered:
FunctionEnd

; The version being replaced, set aside until the new one is in (.onInstSuccess deletes it, .onInstFailed puts it back).
!macro SET_ASIDE part
    RMDir /r "$INSTDIR\${part}.old"
    ${If} ${FileExists} "$INSTDIR\${part}\*.*"
        ClearErrors
        Rename "$INSTDIR\${part}" "$INSTDIR\${part}.old"
        ; A file still held open (by something other than Lexiglance): removed as before instead.
        ${If} ${Errors}
            RMDir /r "$INSTDIR\${part}"
        ${EndIf}
    ${EndIf}
!macroend

!macro PUT_BACK part
    ${If} ${FileExists} "$INSTDIR\${part}.old\*.*"
        RMDir /r "$INSTDIR\${part}"
        Rename "$INSTDIR\${part}.old" "$INSTDIR\${part}"
    ${EndIf}
!macroend

Section
    Call StopRunning
    ; An earlier version's programs go completely, so none of its files linger next to the new ones.
    ${If} ${FileExists} "$INSTDIR\Uninstall.exe"
        !insertmacro SET_ASIDE "bin"
        !insertmacro SET_ASIDE "lib"
        !insertmacro SET_ASIDE "share"
    ${EndIf}
    SetOutPath "$INSTDIR"
    File /r "${STAGE}\*"
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    SetShellVarContext current
    ; The shortcut the settings application keeps too, so there is only ever one.
    CreateShortcut "$SMPROGRAMS\Lexiglance.lnk" "$INSTDIR\bin\lexiglance.exe"

    ; Start with Windows, the settings application's option under the same name: turned on by a first install, and on
    ; an update kept pointing here if it is on (someone who turned it off keeps it off).
    ReadRegStr $1 HKCU "${APP_KEY}" "InstallDir"
    ReadRegStr $2 HKCU "${RUN_KEY}" "Lexiglance"
    ${If} $1 == ""
    ${OrIf} $2 != ""
        WriteRegStr HKCU "${RUN_KEY}" "Lexiglance" '"$INSTDIR\bin\lexiglanced.exe"'
    ${EndIf}
    ; The settings application in the tray at login too, when chosen there: kept pointing here as well.
    ReadRegStr $3 HKCU "${RUN_KEY}" "Lexiglance tray"
    ${If} $3 != ""
        WriteRegStr HKCU "${RUN_KEY}" "Lexiglance tray" '"$INSTDIR\bin\lexiglance.exe" --tray'
    ${EndIf}

    WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Lexiglance"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\bin\lexiglance.exe"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "MattFor"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/MattFor/lexiglance"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" $0

    Call OfferVcRedist
SectionEnd

; Straight into Lexiglance, which starts its daemon. A silent install (CI) starts nothing, unless it is Lexiglance
; updating itself: /relaunch=tray opens it in the tray, /relaunch=window on its updates, /relaunch=daemon only the
; daemon (an update made in the background, with no window open).
Function .onInstSuccess
    RMDir /r "$INSTDIR\bin.old"
    RMDir /r "$INSTDIR\lib.old"
    RMDir /r "$INSTDIR\share.old"
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/relaunch=" $1
    ${IfNot} ${Errors}
        ${If} $1 == "tray"
            Exec '"$INSTDIR\bin\lexiglance.exe" --tray'
        ${ElseIf} $1 == "daemon"
            Exec '"$INSTDIR\bin\lexiglanced.exe"'
        ${Else}
            Exec '"$INSTDIR\bin\lexiglance.exe" --page overview'
        ${EndIf}
    ${Else}
        ${IfNot} ${Silent}
            Exec '"$INSTDIR\bin\lexiglance.exe"'
        ${EndIf}
    ${EndIf}
FunctionEnd

; The version before, back in place of a new one that did not go in whole; and started again as it was, so an update
; that fails leaves Lexiglance running rather than gone.
Function .onInstFailed
    !insertmacro PUT_BACK "bin"
    !insertmacro PUT_BACK "lib"
    !insertmacro PUT_BACK "share"
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/relaunch=" $1
    ${IfNot} ${Errors}
    ${AndIf} ${FileExists} "$INSTDIR\bin\lexiglanced.exe"
        Exec '"$INSTDIR\bin\lexiglanced.exe"'
    ${EndIf}
FunctionEnd

; Silent: everything goes unless /KEEPDATA is given. Asked: the box below, ticked to begin with.
Function un.onInit
    StrCpy $KeepData 0
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/KEEPDATA" $1
    ${IfNot} ${Errors}
        StrCpy $KeepData 1
    ${EndIf}
FunctionEnd

Function un.ChoosePage
    !insertmacro MUI_HEADER_TEXT "Uninstall Lexiglance" "Remove Lexiglance from this computer."
    nsDialogs::Create 1018
    Pop $0
    ${If} $0 == error
        Abort
    ${EndIf}
    ${NSD_CreateLabel} 0 0 100% 36u "Lexiglance will be removed from $INSTDIR, with its Start menu entry and its autostart."
    Pop $0
    ${NSD_CreateCheckbox} 0 44u 100% 12u "Also delete my settings, dictionaries and downloaded models"
    Pop $KeepBox
    ${If} $KeepData == 0
        ${NSD_Check} $KeepBox
    ${EndIf}
    ${NSD_CreateLabel} 12u 60u 95% 30u "Untick to keep them for a later install ($APPDATA\Lexiglance and $LOCALAPPDATA\Lexiglance)."
    Pop $0
    GetDlgItem $0 $HWNDPARENT 1
    SendMessage $0 ${WM_SETTEXT} 0 "STR:Uninstall"
    nsDialogs::Show
FunctionEnd

Function un.ChooseLeave
    ${NSD_GetState} $KeepBox $0
    ${If} $0 == ${BST_CHECKED}
        StrCpy $KeepData 0
    ${Else}
        StrCpy $KeepData 1
    ${EndIf}
FunctionEnd

Section "Uninstall"
    Call un.StopRunning
    SetShellVarContext current
    Delete "$SMPROGRAMS\Lexiglance.lnk"
    DeleteRegValue HKCU "${RUN_KEY}" "Lexiglance"
    DeleteRegValue HKCU "${RUN_KEY}" "Lexiglance tray"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    DeleteRegKey HKCU "${APP_KEY}"
    RMDir /r "$INSTDIR\bin"
    RMDir /r "$INSTDIR\lib"
    RMDir /r "$INSTDIR\share"
    RMDir /r "$INSTDIR\bin.old"
    RMDir /r "$INSTDIR\lib.old"
    RMDir /r "$INSTDIR\share.old"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    ; Settings (%APPDATA%\Lexiglance) and dictionaries, models, logs and the cache (%LOCALAPPDATA%\Lexiglance), unless
    ; they were to be kept for a later install.
    ${If} $KeepData == 0
        RMDir /r "$APPDATA\Lexiglance"
        RMDir /r "$LOCALAPPDATA\Lexiglance"
    ${EndIf}
SectionEnd
