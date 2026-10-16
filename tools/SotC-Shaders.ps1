# Shadow of the Colossus PC - shader warm-up and status console.
#
#   SotC-Warmup.bat  : starts the game windowed and leaves it on the title screen; the attract
#                      demo flies through the world and every new shader gets compiled and cached.
#                      Prints READY once no new shader showed up for -ReadyMinutes minutes.
#   SotC-Play.bat    : starts the game normally and shows the same live status, so a pause is
#                      shown as "COMPILING" (first time a shader is seen) instead of a mystery.
#
# How it works: the port saves every pipeline it builds (user\cache\CUSA08809[_nodma]\*.key) and
# rebuilds all of them at startup ("Preloaded N pipelines"); Intel's driver keeps the compiled
# code in %USERPROFILE%\AppData\LocalLow\Intel\ShaderCache, so known shaders load fast. Only
# shaders never seen before compile during play, and on Intel Arc that can take minutes each.
param(
    [ValidateSet('warmup', 'play')] [string]$Mode = 'play',
    [int]$ReadyMinutes = 5
)

$ErrorActionPreference = 'Stop'
$Port = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $Port 'SotC.exe'
$Log = Join-Path $Port 'user\last_run.log'
$SettingsPath = Join-Path $env:APPDATA 'sotc-launcher\settings.json'
$IntelCache = Join-Path $env:USERPROFILE 'AppData\LocalLow\Intel\ShaderCache'
$Utf8 = New-Object System.Text.UTF8Encoding($false)

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Stamp { (Get-Date).ToString('HH:mm:ss') }
function Ago([double]$seconds) {
    if ($seconds -lt 60) { return ('{0:0}s' -f $seconds) }
    return ('{0:0}m{1:00}s' -f [math]::Floor($seconds / 60), ($seconds % 60))
}
function GameProcesses { @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Port, 'OrdinalIgnoreCase') -and $_.Id -ne $PID }) }
function ReadSettings { [System.IO.File]::ReadAllText($SettingsPath, $Utf8) }
function WriteSettings([string]$json) { [System.IO.File]::WriteAllText($SettingsPath, $json, $Utf8) }

if (-not (Test-Path $Exe)) { Say "SotC.exe not found next to this script ($Port)." Red; exit 1 }
if (-not (Test-Path $SettingsPath)) { Say "Open SotC.exe once first (no launcher settings yet)." Red; exit 1 }

$settings = (ReadSettings) | ConvertFrom-Json
$cacheName = if ($settings.perf_dma -eq $false) { 'CUSA08809_nodma' } else { 'CUSA08809' }
$CacheDir = Join-Path $Port "user\cache\$cacheName"
function CachedPipelines {
    if (-not (Test-Path $CacheDir)) { return 0 }
    return @(Get-ChildItem $CacheDir -Filter '*.key' -File -ErrorAction SilentlyContinue).Count
}

$host.UI.RawUI.WindowTitle = "SotC shaders - $Mode"
try { Start-Transcript -Path (Join-Path $Port 'user\shaders_console.log') -Force | Out-Null } catch {}
Say '=================================================================' DarkCyan
Say "  Shadow of the Colossus PC - shader status ($Mode mode)" Cyan
Say '=================================================================' DarkCyan
$intelMB = 0
if (Test-Path $IntelCache) { $intelMB = [math]::Round(((Get-ChildItem $IntelCache -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum) / 1MB) }
$startCount = CachedPipelines
Say ("  Pipeline cache : {0} pipelines in user\cache\{1}" -f $startCount, $cacheName)
Say ("  Intel driver   : {0} MB of compiled shaders" -f $intelMB)
if ($Mode -eq 'warmup') {
    Say ''
    Say '  The game opens in a window on the title screen. Do NOT press anything in it:' Yellow
    Say '  the attract demo flies through the world and its shaders get compiled.' Yellow
    Say '  Black or frozen picture = a shader is compiling. That is expected; just wait.' Yellow
    Say ("  READY appears once no new shader showed up for {0} minutes." -f $ReadyMinutes) Yellow
}
Say '  Keys here: [Q] close the game and quit' DarkGray
Say ''

if ((GameProcesses).Count -gt 0) {
    Say '  The game (or the launcher) is already running. Close it first, then run this again.' Red
    Read-Host '  Press Enter to exit' | Out-Null
    exit 1
}

# Warm-up runs windowed and without mouse capture, so the PC stays usable meanwhile.
$originalSettings = $null
if ($Mode -eq 'warmup') {
    $originalSettings = ReadSettings
    $s = $originalSettings | ConvertFrom-Json
    $s.fullscreen = $false
    $s.mouse = $false
    WriteSettings ($s | ConvertTo-Json -Depth 8)
}

# Info-level GPU log: "Preloaded N pipelines" and one "Compiling ... pipeline" line per new one.
$env:BB_GPU_LOG = 'info'
$launchTime = Get-Date
Start-Process -FilePath $Exe -ArgumentList '--play' -WorkingDirectory $Port | Out-Null
Say "[$(Stamp)] Game starting..." Green

try {
    # The log is rewritten on each launch: wait until it is the new one.
    $deadline = (Get-Date).AddSeconds(60)
    while (-not ((Test-Path $Log) -and (Get-Item $Log).LastWriteTime -gt $launchTime)) {
        if ((Get-Date) -gt $deadline) { throw 'The game did not start (no new user\last_run.log after 60 s).' }
        Start-Sleep -Milliseconds 500
    }
    if ($originalSettings) { WriteSettings $originalSettings; $originalSettings = $null }

    $offset = 0L
    $newPipelines = 0
    $lastNew = Get-Date
    $lastFrame = $null
    $fps = 0.0
    $preloadShown = $false
    $ready = $false
    $state = 'LOADING'
    $lastStatus = [datetime]::MinValue
    $exited = $false

    while (-not $exited) {
        # New log lines (the game keeps the file open, so share it).
        $fs = [System.IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
        try {
            if ($fs.Length -lt $offset) { $offset = 0 }
            $fs.Seek($offset, 'Begin') | Out-Null
            $reader = New-Object System.IO.StreamReader($fs, $Utf8)
            $chunk = $reader.ReadToEnd()
            $offset = $fs.Position
        } finally { $fs.Dispose() }

        foreach ($line in ($chunk -split "`n")) {
            if ($line -match 'Preloading pipelines: (\d+)/(\d+)( on \d+ threads)?') {
                $pct = if ([int]$Matches[2] -gt 0) { 100 * [int]$Matches[1] / [int]$Matches[2] } else { 0 }
                Say ("[{0}] Preloading shaders: {1}/{2} ({3:0}%){4}" -f (Stamp), $Matches[1], $Matches[2], $pct, $Matches[3]) Cyan
            } elseif ($line -match 'Preloaded (\d+) pipelines in (\d+) ms') {
                $preloadShown = $true
                Say ("[{0}] Preloaded {1} cached pipelines in {2:0.0} s" -f (Stamp), $Matches[1], ($Matches[2] / 1000)) Green
            } elseif ($line -match 'Compiling (graphics|compute) pipeline') {
                $newPipelines++
                $lastNew = Get-Date
                if ($ready) {
                    $ready = $false
                    Say "[$(Stamp)] New shader appeared - not ready yet, keep waiting." Yellow
                }
            } elseif ($line -match 'SOTC FPS ([\d.]+) at') {
                $fps = [double]$Matches[1]
                # READY counts game time only, not the startup preload.
                if (-not $lastFrame -and $newPipelines -eq 0) { $lastNew = Get-Date }
                $lastFrame = Get-Date
            } elseif ($line -match 'Vulkan device lost|\[exit code') {
                if ($line -match 'device lost') { Say "[$(Stamp)] GPU error: Vulkan device lost (see user\last_run.log)." Red }
                $exited = $true
            }
        }
        if ((GameProcesses).Count -eq 0) { $exited = $true }
        if ($exited) { break }

        $now = Get-Date
        $sinceNew = ($now - $lastNew).TotalSeconds
        # The game logs FPS every 5 s; allow for this console reading the log late under load.
        $stalled = $lastFrame -and (($now - $lastFrame).TotalSeconds -gt 12)
        if (-not $lastFrame) { $state = 'LOADING' }
        elseif ($stalled -and $sinceNew -lt 120) { $state = 'COMPILING' }
        elseif ($stalled) { $state = 'PAUSED' }
        else { $state = 'RUNNING' }

        if (($now - $lastStatus).TotalSeconds -ge 5) {
            $lastStatus = $now
            $color = switch ($state) { 'RUNNING' { 'Gray' } 'COMPILING' { 'Yellow' } default { 'DarkYellow' } }
            $note = switch ($state) {
                'COMPILING' { '  <- picture frozen: compiling a new shader, do not close' }
                'LOADING' { '  <- starting up / preloading shaders' }
                'PAUSED' { '  <- no frames (game paused, minimized or compiling)' }
                default { '' }
            }
            $fpsText = if ($state -eq 'RUNNING') { '{0,5:0.0}' -f $fps } else { '    -' }
            Say ("[{0}] {1,-9} FPS {2} | cached {3,5} | new this run {4,4} | last new shader {5,7} ago{6}" -f `
                (Stamp), $state, $fpsText, (CachedPipelines), $newPipelines, (Ago $sinceNew), $note) $color
        }

        if (-not $ready -and $lastFrame -and $state -eq 'RUNNING' -and $sinceNew -ge ($ReadyMinutes * 60)) {
            $ready = $true
            Say ''
            Say '#################################################################' Green
            if ($Mode -eq 'warmup') {
                Say ("  READY: no new shader for {0} min. Everything the demo shows is cached." -f $ReadyMinutes) Green
                Say '  Press Q to close the game, then play with SotC-Play.bat.' Green
                Say '  (Areas the demo never visits can still pause once, the first time.)' DarkGreen
            } else {
                Say ("  READY: no new shader for {0} min - this part of the game is cached." -f $ReadyMinutes) Green
            }
            Say '#################################################################' Green
            Say ''
            [console]::Beep(880, 300)
        }

        if ([console]::KeyAvailable) {
            $key = [console]::ReadKey($true)
            if ($key.Key -eq 'Q') {
                Say "[$(Stamp)] Closing the game..." Cyan
                GameProcesses | Stop-Process -Force -ErrorAction SilentlyContinue
                break
            }
        }
        Start-Sleep -Seconds 1
    }
} finally {
    if ($originalSettings) { WriteSettings $originalSettings }
}

$endCount = CachedPipelines
Say ''
Say ("[{0}] Game closed. Cache: {1} pipelines ({2} new this run)." -f (Stamp), $endCount, ($endCount - $startCount)) Cyan
Read-Host 'Press Enter to close this window' | Out-Null
