; The Windows installer: for the current user, without administrator rights, into %LOCALAPPDATA%\Programs\Lexiglance.
; One window with a progress bar, then Lexiglance opens. Running a newer one updates in place; settings and dictionaries
; live elsewhere and stay. Built from an install prefix by .github/scripts/windows-installer.ps1:
;   makensis /DVERSION=x.y.z /DSTAGE=<prefix> /DOUTFILE=<setup.exe> installer.nsi

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
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
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; The programs cannot be replaced or removed while they run.
!macro STOP_RUNNING prefix
    Function ${prefix}StopRunning
        nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM lexiglance.exe'
        Pop $0
        nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM lexiglanced.exe'
        Pop $0
        Sleep 500
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

Section
    Call StopRunning
    ; An earlier version's programs go completely, so none of its files linger next to the new ones.
    ${If} ${FileExists} "$INSTDIR\Uninstall.exe"
        RMDir /r "$INSTDIR\bin"
        RMDir /r "$INSTDIR\lib"
        RMDir /r "$INSTDIR\share"
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
; updating itself: /relaunch=tray opens it in the tray, /relaunch=window on its updates.
Function .onInstSuccess
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/relaunch=" $1
    ${IfNot} ${Errors}
        ${If} $1 == "tray"
            Exec '"$INSTDIR\bin\lexiglance.exe" --tray'
        ${Else}
            Exec '"$INSTDIR\bin\lexiglance.exe" --page overview'
        ${EndIf}
    ${Else}
        ${IfNot} ${Silent}
            Exec '"$INSTDIR\bin\lexiglance.exe"'
        ${EndIf}
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
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    ; Settings and dictionaries (%APPDATA%\Lexiglance, %LOCALAPPDATA%\Lexiglance) stay for a later install.
SectionEnd
