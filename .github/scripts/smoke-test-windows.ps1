# Checks a Windows package on its own: the .zip cpack makes, unpacked, or the setup .exe, installed silently and
# uninstalled at the end. With nothing but Windows on PATH, the Qt plugins are there, the programs start, a dictionary
# imports and a conjugated word is found. Used by CI and the release.
#   powershell -File .github/scripts/smoke-test-windows.ps1 <package.zip | setup.exe>
# The installer check registers and unregisters Lexiglance for the current user (Start menu, Apps list, autostart):
# run it where Lexiglance is not installed, as CI does.
param([Parameter(Mandatory = $true)][string]$Package)

$ErrorActionPreference = 'Stop'
$work = Join-Path ([IO.Path]::GetTempPath()) ('lexiglance-smoke-' + [Guid]::NewGuid().ToString('N'))
$failures = 0
function Check([string]$What, [bool]$Ok) {
    if ($Ok) { "ok    $What" } else { "FAIL  $What"; $script:failures++ }
}

$installer = $Package.EndsWith('.exe')
$shortcut  = Join-Path ([Environment]::GetFolderPath('Programs')) 'Lexiglance.lnk'
$runKey    = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
function Autostart { $null -ne (Get-ItemProperty $runKey -Name Lexiglance -ErrorAction SilentlyContinue) }
if ($installer) {
    # A silent first install into a folder of its own (/D comes last and unquoted). The .zip checked before may have
    # left a shortcut to itself; the installer has to make its own.
    Remove-Item $shortcut -ErrorAction SilentlyContinue
    $target = Join-Path $work 'installed'
    $setup  = Start-Process $Package -ArgumentList "/S /D=$target" -Wait -PassThru
    Check 'the installer runs silently' ($setup.ExitCode -eq 0)
    Check 'the installer adds a Start menu entry' (Test-Path $shortcut)
    Check 'the installer starts Lexiglance with Windows' (Autostart)
} else {
    Expand-Archive -Path $Package -DestinationPath $work
}
$bin  = (Get-ChildItem $work -Recurse -Filter lexiglanced.exe | Select-Object -First 1).DirectoryName
$root = Split-Path $bin -Parent

# Nothing of the build environment (MSYS2) may help the programs find what they load.
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
# A portable home, so the test leaves the user's settings and dictionaries alone.
$env:LEXIGLANCE_HOME = Join-Path $work 'home'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

foreach ($plugin in 'platforms\qwindows.dll', 'styles\*.dll', 'imageformats\*.dll', 'tls\qschannelbackend.dll') {
    Check "Qt plugin $plugin" (@(Get-ChildItem (Join-Path $root "share\qt6\plugins\$plugin") -ErrorAction SilentlyContinue).Count -gt 0)
}
Check 'qt.conf next to the programs' (Test-Path (Join-Path $bin 'qt.conf'))

# lexiglancectl is a console program. (The words are spelled as code points: this script stays ASCII.)
$fixture   = Join-Path $PSScriptRoot '..\..\tests\data\fixture'
$conjugated = -join [char[]](0x98DF, 0x3079, 0x3055, 0x305B, 0x3089, 0x308C, 0x306A, 0x304B, 0x3063, 0x305F)
$lemma      = -join [char[]](0x98DF, 0x3079, 0x308B)
& "$bin\lexiglancectl.exe" import $fixture | Out-Null
Check 'lexiglancectl imports a dictionary' ($LASTEXITCODE -eq 0)
$found = (& "$bin\lexiglancectl.exe" lookup $conjugated) -join "`n"
Check 'a conjugated word is found' ($LASTEXITCODE -eq 0 -and $found.Contains($lemma))

# The daemon and the settings application are windowed programs: they are waited for. A missing DLL fails at start.
$daemon = Start-Process "$bin\lexiglanced.exe" -ArgumentList '--version' -Wait -PassThru
Check 'the daemon starts' ($daemon.ExitCode -eq 0)
$shot     = Join-Path $work 'about.png'
$settings = Start-Process "$bin\lexiglance.exe" -ArgumentList '--screenshot', 'about', "`"$shot`"", '100' -Wait -PassThru
Check 'the settings application starts and draws a page' ($settings.ExitCode -eq 0 -and (Test-Path $shot) -and (Get-Item $shot).Length -gt 10000)

if ($installer) {
    # _? runs the uninstaller in place and waits for it, instead of starting a copy of it and returning at once.
    $removal = Start-Process (Join-Path $target 'Uninstall.exe') -ArgumentList "/S _?=$target" -Wait -PassThru
    Check 'the uninstaller removes the programs and the Start menu entry' ($removal.ExitCode -eq 0 -and -not (Test-Path (Join-Path $target 'bin')) -and -not (Test-Path $shortcut))
    Check 'the uninstaller stops starting Lexiglance with Windows' (-not (Autostart))
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if ($failures -gt 0) {
    Write-Output "$failures checks failed"
    exit 1
}
Write-Output 'the package works on its own'
