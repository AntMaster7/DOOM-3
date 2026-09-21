# Scripted timedemo run. Appends one result block to bench.txt (append-only: never delete it).
#
#   tools\bench-timedemo.ps1                                   GL baseline, 1920x1080, demo1
#   tools\bench-timedemo.ps1 -Label skipbackend -Extra '+set r_skipBackEnd 1'
#   tools\bench-timedemo.ps1 -Reps 3
#
# Uses its own fs_savepath (build\save-auto) so the user's config and saves are never touched.
# Texture settings default to the matched set of plan section 6.10 (uncompressed, no DDS); pass
# -StockTextures for the engine defaults.
#
# The fixed settings go into build\save-auto\base\autoexec.cfg, not onto the command line: the
# engine keeps at most 32 "+" entries (MAX_CONSOLE_LINES) and silently drops the rest, which
# would be the trailing +timeDemoQuit.
param(
    [string]$Label = 'gl',
    [string]$Platform = 'Win32',
    [string]$Config = 'Release',
    [int]$Width = 1920,
    [int]$Height = 1080,
    [string]$Demo = 'demo1',
    [string]$Extra = '',
    [string]$Command = '',
    [int]$Reps = 1,
    [switch]$StockTextures,
    [switch]$Twice,          # untimed warm-up pass first: the timed pass has everything loaded
    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# SW_EXE_NAME: bench a renamed copy of the engine, to interleave two BUILDS in one session (compile-time A/Bs)
$exeName = if ($env:SW_EXE_NAME) { $env:SW_EXE_NAME } else { 'DOOM3.exe' }
$exe = Join-Path $root "build\$Platform\$Config\$exeName"
$basePath = 'C:\Program Files (x86)\Steam\steamapps\common\Doom 3'
$savePath = Join-Path $root 'build\save-auto'
$benchLog = Join-Path $root 'bench.txt'
if ($Command -eq '') {
    $Command = "+timeDemoQuit $Demo"
    if ($Twice) { $Command += ' twice' }
}
$passes = 'single pass'
if ($Twice) { $passes = 'warm (second pass timed)' }
$crashFile = Join-Path (Split-Path $exe) 'crash.txt'

if (-not (Test-Path $exe)) { throw "missing $exe" }
New-Item -ItemType Directory -Force (Join-Path $savePath 'base') | Out-Null

# A savepath without config.spec makes the engine run its machine-spec detection ("qualifies for
# Low quality" on any modern GPU: Sys_GetVideoRam reports -1) and overwrite the image cvars AFTER
# the command line was applied. An empty config.spec switches that off.
$spec = Join-Path $savePath 'base\config.spec'
if (-not (Test-Path $spec)) { New-Item -ItemType File $spec | Out-Null }

$texName = 'tex=uncompressed'
$tex = @('image_usePrecompressedTextures 0', 'image_useCompression 0', 'image_useNormalCompression 0')
if ($StockTextures) {
    $texName = 'tex=stock(dds)'
    $tex = @('image_usePrecompressedTextures 1', 'image_useCompression 1', 'image_useNormalCompression 2')
}
$settings = @(
    'com_allowConsole 1', 'r_fullscreen 0', 'r_mode -1', "r_customWidth $Width", "r_customHeight $Height",
    # sound stays on: light materials read sound amplitude (idSoundWorldLocal::FindAmplitude), so
    # s_noSound 1 would change both the image and the front-end cost. timeDemo mutes the output.
    'r_swapInterval 0', 'r_multiSamples 0', 's_noSound 0', 'in_mouse 0', 'win_allowMultipleInstances 1',
    # the High-quality set (com_machineSpec 2) minus anisotropy
    'com_machineSpec 2', 'image_downSize 0', 'image_downSizeBump 0', 'image_downSizeSpecular 0',
    'image_forceDownSize 0', 'image_ignoreHighQuality 0', 'image_roundDown 1', 'image_lodbias 0',
    'image_anisotropy 1', 'image_preload 1', 'image_useCache 0', 'image_filter GL_LINEAR_MIPMAP_LINEAR',
    # ARCHIVED debug cvars: an -Extra of one run would otherwise live on in DoomConfig.cfg (it did, 2026-09-21)
    'r_shadows 1', 'r_skipBump 0', 'r_skipSpecular 0', 'r_skipDiffuse 0',
    # archived too, and r_mode -1 never resets it: a run at a 16:9 mode of the table (r_mode 9 - 12
    # sets it to 1) would change the field of view, and with it every number, from then on
    'r_aspectRatio 0'
) + $tex
$cfg = ($settings | ForEach-Object { "seta $_" }) -join "`r`n"
[System.IO.File]::WriteAllText((Join-Path $savePath 'base\autoexec.cfg'), $cfg + "`r`n")

$cpu = (Get-CimInstance Win32_Processor).Name.Trim()
$threads = (Get-CimInstance Win32_Processor).NumberOfLogicalProcessors
$gpu = ((Get-CimInstance Win32_VideoController).Name | Select-Object -First 1)

for ($rep = 1; $rep -le $Reps; $rep++) {
    $foreign = @(Get-Process DOOM3, renderer -ErrorAction SilentlyContinue)
    if ($foreign.Count -gt 0) { throw "another DOOM3/renderer process is running: a bench now is worthless" }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $logName = "bench-$Label-$stamp.log"
    $argLine = "+set fs_basepath `"$basePath`" +set fs_savepath `"$savePath`" " +
        "+set logFile 2 +set logFileName $logName $Extra $Command"

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $tokens = [regex]::Matches($argLine, '"[^"]*"|\S+').Count
    if ($tokens -gt 64) { throw "the command line has $tokens tokens; the engine keeps 64 (idCmdArgs::MAX_COMMAND_ARGS) and drops the rest silently" }
    $p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path $exe) -PassThru
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $p.Kill()
        throw "timed out after $TimeoutSec s"
    }
    $sw.Stop()

    if ((Test-Path $crashFile) -and ((Get-Item $crashFile).LastWriteTime -gt (Get-Date).AddSeconds(-$sw.Elapsed.TotalSeconds - 2))) {
        Get-Content $crashFile | Write-Output
        throw "the engine crashed (exit code $($p.ExitCode)): see $crashFile"
    }

    $logPath = Join-Path $savePath "base\$logName"
    if (-not (Test-Path $logPath)) { throw "no console log at $logPath (exit code $($p.ExitCode))" }
    $resultLine = Select-String -Path $logPath -Pattern 'frames rendered in' | Select-Object -Last 1
    if (-not $resultLine) { throw "no timedemo result in $logPath (exit code $($p.ExitCode), $([int]$sw.Elapsed.TotalSeconds) s)" }

    $header = "--- $stamp | $cpu | threads $threads | $gpu | $Platform $Config | ${Width}x${Height} windowed | " +
        "$Demo $passes | label=$Label | $texName | extra=[$Extra] | wall $([int]$sw.Elapsed.TotalSeconds) s"
    Add-Content -Path $benchLog -Value $header -Encoding utf8
    Add-Content -Path $benchLog -Value $resultLine.Line.Trim() -Encoding utf8
    Write-Output $header
    Write-Output $resultLine.Line.Trim()
    # the software renderer's per-phase report (printed at shutdown: the last 2148 frames = the timed pass)
    foreach ($line in (Select-String -Path $logPath -Pattern '^swstats')) {
        Add-Content -Path $benchLog -Value ('    ' + $line.Line.Trim()) -Encoding utf8
        Write-Output ('    ' + $line.Line.Trim())
    }
}
