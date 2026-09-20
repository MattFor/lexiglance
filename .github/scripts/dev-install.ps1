# Builds Lexiglance and puts it over the copy installed on this machine, so a change can be tried on your own desktop
# without going through the installer. Windows cannot overwrite a running program, so whatever is running is stopped
# first and started again afterwards, exactly as the installer does.
#   pwsh .github/scripts/dev-install.ps1 [-Preset release] [-Prefix <dir>] [-SkipBuild] [-NoRestart]
# Without -Prefix it goes where Lexiglance is started from at login (the Run entry), else where the installer put it
# (HKCU\Software\Lexiglance, else %LOCALAPPDATA%\Programs\Lexiglance), which keeps the Start menu shortcut and the
# autostart entries pointing at the copy that actually runs.
[CmdletBinding()]
param(
    [string]$Preset = 'release',
    [string]$Prefix,
    [switch]$SkipBuild,
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Invoke-Checked {
    & $args[0] @($args[1..($args.Length - 1)])
    if ($LASTEXITCODE -ne 0) {
        throw "$($args[0]) failed with exit code $LASTEXITCODE"
    }
}

Push-Location $repository
try {
    if (-not $Prefix) {
        # The copy the login autostart starts: <prefix>\bin\lexiglanced.exe.
        $run = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Lexiglance' -ErrorAction SilentlyContinue).Lexiglance
        if ($run) {
            $daemon = $run.Trim().Trim('"')
            if ($daemon -like '*\bin\lexiglanced.exe') {
                $Prefix = Split-Path (Split-Path $daemon -Parent) -Parent
                Write-Host "the autostart runs $daemon"
            }
        }
    }
    if (-not $Prefix) {
        $Prefix = (Get-ItemProperty 'HKCU:\Software\Lexiglance' -Name 'InstallDir' -ErrorAction SilentlyContinue).InstallDir
    }
    if (-not $Prefix) {
        $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\Lexiglance'
    }

    $build = Join-Path $repository "build\$Preset"
    if (-not $SkipBuild) {
        if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
            Invoke-Checked cmake '--preset' $Preset
        }
        Invoke-Checked cmake '--build' '--preset' $Preset
    }
    if (-not (Test-Path (Join-Path $build 'apps\daemon\lexiglanced.exe'))) {
        throw "no build in $build - run without -SkipBuild"
    }

    # The same things come back up as were running, and nothing else.
    $was = @{}
    foreach ($name in 'lexiglanced', 'lexiglance') {
        $processes = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        $was[$name] = $processes.Count -gt 0
        if ($was[$name]) {
            Write-Host "stopping $name..."
            $processes | Stop-Process -Force
            $processes | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
        }
    }

    Write-Host "installing into $Prefix..."
    Invoke-Checked cmake '--install' $build '--prefix' $Prefix

    if ($NoRestart) {
        Write-Host 'not restarting (-NoRestart).'
    }
    elseif (-not ($was['lexiglanced'] -or $was['lexiglance'])) {
        Write-Host "nothing was running; start it with $Prefix\bin\lexiglance.exe"
    }
    else {
        # The daemon first: the settings application talks to it over the pipe as soon as it opens.
        if ($was['lexiglanced']) {
            Start-Process (Join-Path $Prefix 'bin\lexiglanced.exe')
        }
        if ($was['lexiglance']) {
            Start-Process (Join-Path $Prefix 'bin\lexiglance.exe') -ArgumentList '--tray'
        }
        Write-Host 'restarted.'
    }

    Write-Output (Join-Path $Prefix 'bin')
}
finally {
    Pop-Location
}
