# Builds the Windows installer (lexiglance-<version>-windows-setup.exe) with NSIS from what `cmake --install` put in a
# prefix. It installs for the current user only and needs no administrator rights. Used by CI and the release.
#   pwsh .github/scripts/windows-installer.ps1 <build directory> <install prefix> <output directory>
param(
    [Parameter(Mandatory = $true)][string]$Build,
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = 'Stop'
$makensis = Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'
if (-not (Test-Path $makensis)) {
    choco install nsis --no-progress -y | Out-Null
}

$version = (Select-String -Path (Join-Path $Build 'CMakeCache.txt') -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$').Matches[0].Groups[1].Value
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$setup = Join-Path (Resolve-Path $Output).Path "lexiglance-$version-windows-setup.exe"
$script = Join-Path $PSScriptRoot '..\..\data\windows\installer.nsi'

& $makensis /V2 "/DVERSION=$version" "/DSTAGE=$((Resolve-Path $Prefix).Path)" "/DOUTFILE=$setup" $script
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
Write-Output $setup
