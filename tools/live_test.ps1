<#
Manual live-test session for HZDRawMouse, meant to run as a job of the shared queue
(D:\Githubs\_agents\queue_executor, see its AGENTS.md):

  1. refuses to start if the game is already running (a session outside the queue)
  2. archives the previous plugin logs, deploys build\HZDRawMouse.asi + config (deploy.bat)
  3. launches Horizon Zero Dawn through Steam and waits for the user to quit it
  4. archives this session's logs under research\logs\<timestamp>_<Label>\ and prints a summary

If the session runs past -SessionMinutes the game is asked to close (and ended a minute later),
so the next queued job never overlaps with it.
#>
param(
    [string]$Label = 'session',
    [double]$SessionMinutes = 45,
    [switch]$NoDeploy,
    # Install a release zip (as a user would: extract everything into the game folder) instead of
    # deploying the development build.
    [string]$PackageZip = ''
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$gameDir = 'D:\Program Files\Games\Steam\steamapps\common\Horizon Zero Dawn'
$steamAppId = 1151640
$processName = 'HorizonZeroDawn'
$logFiles = @('HZDRawMouse.log', 'HZDRawMouse_events.csv')

function Write-Step([string]$text) { Write-Output "[$((Get-Date).ToString('HH:mm:ss'))] $text" }

function Save-Logs([string]$suffix) {
    $present = @($logFiles | Where-Object { Test-Path -LiteralPath (Join-Path $gameDir $_) })
    if (-not $present.Count) { return $null }
    $safe = ($Label -replace '[^\w\-]+', '_')
    $dest = Join-Path $repo ("research\logs\{0}_{1}{2}" -f (Get-Date -Format 'yyyy-MM-dd_HHmmss'), $safe, $suffix)
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($f in $present) { Copy-Item -LiteralPath (Join-Path $gameDir $f) -Destination $dest }
    return $dest
}

if (Get-Process $processName -ErrorAction SilentlyContinue) {
    Write-Step "$processName is already running outside the queue - not starting a second session"
    exit 2
}

$previous = Save-Logs '_previous'
if ($previous) { Write-Step "previous logs archived to $previous" }

if ($PackageZip) {
    if (-not (Test-Path -LiteralPath $PackageZip)) { Write-Step "package not found: $PackageZip"; exit 3 }
    # Clean install as the release README describes: the loader (if bundled), the plugin and its
    # ini go beside the exe. Development leftovers (our dev UAL ini) are removed first.
    Remove-Item -LiteralPath (Join-Path $gameDir 'winmm.ini') -ErrorAction SilentlyContinue
    $unpack = Join-Path $env:TEMP ("hzdrm-package-" + [guid]::NewGuid().ToString('N'))
    Expand-Archive -LiteralPath $PackageZip -DestinationPath $unpack -Force
    foreach ($f in @('winmm.dll', 'HZDRawMouse.asi', 'HZDRawMouse.ini')) {
        $src = Join-Path $unpack $f
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $gameDir -Force; Write-Step "installed $f" }
    }
    Remove-Item -LiteralPath $unpack -Recurse -Force
    Write-Step "installed release package $(Split-Path -Leaf $PackageZip) into the game folder"
} elseif (-not $NoDeploy) {
    & cmd.exe /d /c "`"$repo\deploy.bat`""
    if ($LASTEXITCODE -ne 0) { Write-Step "deploy failed ($LASTEXITCODE)"; exit 3 }
}

Write-Step "launching Horizon Zero Dawn (steam://rungameid/$steamAppId) - label '$Label'"
Start-Process "steam://rungameid/$steamAppId"

# Steam starts the game itself (not as our child): find it, then wait for it to exit.
$game = $null
$launchDeadline = (Get-Date).AddMinutes(3)
while (-not $game -and (Get-Date) -lt $launchDeadline) {
    Start-Sleep -Seconds 2
    $game = Get-Process $processName -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $game) { Write-Step 'the game did not start within 3 minutes'; exit 4 }
$gameStarted = Get-Date
Write-Step "game running (PID $($game.Id)); waiting for the user to quit (session limit $SessionMinutes min)"

$sessionDeadline = (Get-Date).AddMinutes($SessionMinutes)
$timedOut = $false
while (-not $game.HasExited) {
    if ((Get-Date) -gt $sessionDeadline) {
        $timedOut = $true
        Write-Step 'session limit reached - asking the game to close'
        $game.CloseMainWindow() | Out-Null
        if (-not $game.WaitForExit(60000)) {
            Write-Step 'still running after 60 s - ending it'
            Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
        }
        break
    }
    Start-Sleep -Seconds 2
}
# Let the plugin's worker flush its files.
Start-Sleep -Seconds 3
Write-Step "game exited after $([math]::Round(((Get-Date) - $gameStarted).TotalMinutes, 1)) min"

$saved = Save-Logs ''
# Crash reports written by Ultimate ASI Loader (only when <game>\CrashDumps exists).
$crashDir = Join-Path $gameDir 'CrashDumps'
$crashes = @(Get-ChildItem -LiteralPath $crashDir -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $gameStarted.AddMinutes(-1) })
if ($crashes.Count) {
    if (-not $saved) { $saved = Join-Path $repo ("research\logs\{0}_{1}" -f (Get-Date -Format 'yyyy-MM-dd_HHmmss'), ($Label -replace '[^\w\-]+', '_')); New-Item -ItemType Directory -Force $saved | Out-Null }
    foreach ($c in $crashes) { Copy-Item -LiteralPath $c.FullName -Destination $saved }
    Write-Step "CRASH REPORT(S) found: $(($crashes | ForEach-Object Name) -join ', ')"
    foreach ($c in ($crashes | Where-Object Extension -eq '.log')) {
        Write-Output "--- $($c.Name) (first 40 lines)"
        Get-Content -LiteralPath $c.FullName -TotalCount 40
    }
}
if ($saved) {
    Write-Step "session logs archived to $saved"
    $log = Join-Path $saved 'HZDRawMouse.log'
    if (Test-Path -LiteralPath $log) {
        Write-Output '--- plugin log: hook status, mode/debug changes, markers'
        Select-String -LiteralPath $log -Pattern 'RawCam:|RawCam DEBUG|Mouse mode|MARK|NOT found|disabled|FAILED|watch: (armed|disarmed)' |
            ForEach-Object { $_.Line }
    }
} else {
    Write-Step 'no plugin logs found (did the plugin load?)'
}
if ($timedOut) { exit 124 }
exit 0
