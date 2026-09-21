# ARCHIVED cvars persist in build\save-auto between runs (r_shadows 0 from one -Extra poisoned the next five runs):
# whatever a run may change is pinned below, and -Extra comes last so it still wins.
# Runs the x64 engine through a console script and collects its log: the generic scripted run of
# the software renderer work. The script is a .cfg (one command per line, `wait N` between them);
# screenshots land in build\save-auto\base\screenshots. Never touches build\save.
#
#   tools\sw-run.ps1 -Tag menu-sw -Sw -Script @('wait 120', 'screenshot "screenshots/menu-sw.tga"')
#   tools\sw-run.ps1 -Tag menu-gl      -Script @('wait 120', 'screenshot "screenshots/menu-gl.tga"')
#   -Sw            +set r_swRenderer 1
#   -Compare       with -Sw: +set r_swCompare 1, the GL back end in the same process on the same images
#   -NoQuit        do not append `quit` (the script ends the run itself: timeDemoQuit)
#   -Show pattern  print the log lines that match
param(
    [string]$Tag = 'run',
    [string[]]$Script = @('wait 120'),
    [switch]$Sw,
    [switch]$Compare,
    [switch]$NoQuit,
    [string]$Extra = '',
    [string]$Show = '',
    [int]$Width = 1920,
    [int]$Height = 1080,
    [int]$TimeoutSec = 300,
    [string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$savePath = Join-Path $root 'build\save-auto'
# SW_EXE_NAME: run a renamed copy of the engine (GPU drivers key application profiles on the exe name)
$exeName = if ($env:SW_EXE_NAME) { $env:SW_EXE_NAME } else { 'DOOM3.exe' }
$exe = Join-Path $root "build\$Platform\Release\$exeName"
if (-not (Test-Path (Join-Path $savePath 'base\autoexec.cfg'))) { throw 'run tools\bench-timedemo.ps1 once first: it writes autoexec.cfg' }
# -NoQuit: the script ends the run itself (timeDemoQuit)
$lines = @($Script)
if (-not $NoQuit) { $lines += @('wait 2', 'echo SWRUN_DONE', 'quit') }
[System.IO.File]::WriteAllText((Join-Path $savePath 'base\sw-run.cfg'), ($lines -join "`r`n") + "`r`n")
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logName = "swrun-$Tag-$stamp.log"
$crashFile = Join-Path (Split-Path $exe) 'crash.txt'
$mode = ''
if ($Sw) { $mode = '+set r_swRenderer 1 ' }
if ($Sw -and $Compare) { $mode += '+set r_swCompare 1 ' }

# THE COMMAND LINE HOLDS 64 TOKENS, NOT MORE. On Windows the engine tokenizes the whole line into
# one idCmdArgs (MAX_COMMAND_ARGS = 64) and drops the rest WITHOUT A WORD: 21 "+set a b" are 63
# tokens, "+exec" the 64th, and the script's file name was gone. The engine then sat at its
# console printing "exec <filename> : execute a script file" until the timeout (2026-09-21; the
# user found the window). So every fixed setting lives in autoexec.cfg, which this script rewrites
# on each run (bench-timedemo.ps1 does the same with its own set; runs never overlap). autoexec
# runs before the command line's +set are applied again, so -Extra still wins.
$settings = @(
    'com_allowConsole 1', 'r_fullscreen 0', 'r_mode -1', "r_customWidth $Width", "r_customHeight $Height",
    'r_swapInterval 0', 'r_multiSamples 0', 's_noSound 0', 'in_mouse 0', 'win_allowMultipleInstances 1',
    'com_skipIntroVideos 1', 's_constantAmplitude 1',
    # the High-quality texture set, uncompressed, with the software path's filter: GL matched to it
    'com_machineSpec 2', 'image_downSize 0', 'image_downSizeBump 0', 'image_downSizeSpecular 0',
    'image_forceDownSize 0', 'image_ignoreHighQuality 0', 'image_roundDown 1', 'image_lodbias 0',
    'image_anisotropy 1', 'image_preload 1', 'image_useCache 0', 'image_filter GL_LINEAR_MIPMAP_NEAREST',
    'image_usePrecompressedTextures 0', 'image_useCompression 0', 'image_useNormalCompression 0',
    # ARCHIVED debug cvars: an -Extra of one run lives on in DoomConfig.cfg otherwise
    'r_shadows 1', 'r_skipBump 0', 'r_skipSpecular 0', 'r_skipDiffuse 0',
    # archived too, and r_mode -1 never resets it: one run at a 16:9 mode of the table (r_mode 9 - 12
    # sets it to 1) would change the field of view, i.e. every frame of the gate, from then on
    'r_aspectRatio 0'
)
[System.IO.File]::WriteAllText((Join-Path $savePath 'base\autoexec.cfg'), (($settings | ForEach-Object { "seta $_" }) -join "`r`n") + "`r`n")

$argLine = "+set fs_basepath `"C:\Program Files (x86)\Steam\steamapps\common\Doom 3`" +set fs_savepath `"$savePath`" " +
    "+set logFile 2 +set logFileName $logName $mode$Extra +exec sw-run.cfg"
$tokens = [regex]::Matches($argLine, '"[^"]*"|\S+').Count
if ($tokens -gt 64) { throw "the command line has $tokens tokens; the engine keeps 64 and drops the rest silently (move settings into the script or autoexec.cfg)" }
$sw1 = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path $exe) -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "TIMED OUT after $TimeoutSec s: killed" }
$sw1.Stop()
if ((Test-Path $crashFile) -and ((Get-Item $crashFile).LastWriteTime -gt (Get-Date).AddSeconds(-$sw1.Elapsed.TotalSeconds - 2))) {
    Get-Content $crashFile | Select-Object -First 30
    throw "the engine crashed (exit code $($p.ExitCode))"
}
$log = Get-Content (Join-Path $savePath "base\$logName")
$done = @($log | Where-Object { $_ -match 'SWRUN_DONE' }).Count -gt 0
$errors = @($log | Where-Object { $_ -match '^ERROR:|^\*\*\*\*|Error:' })
# WARNING lines are part of the summary: 336 "couldn't find font" per run went unseen for hours (2026-09-21)
$warnings = @($log | Where-Object { $_ -match '^WARNING:' })
Write-Output ("sw-run {0}: done={1}, exit {2}, {3} s, {4} error lines, {5} warnings, log {6}" -f $Tag, $done, $p.ExitCode, [int]$sw1.Elapsed.TotalSeconds, $errors.Count, $warnings.Count, $logName)
if ($warnings.Count -gt 20) { $warnings | Group-Object | Sort-Object Count -Descending | Select-Object -First 3 | ForEach-Object { "   {0}x {1}" -f $_.Count, $_.Name } }
$errors | Select-Object -First 10
if ($Show) { $log | Where-Object { $_ -match $Show } | Select-Object -First 60 }
