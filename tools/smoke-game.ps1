# Smoke test of the GAME (not a demo): loads a map, lets it run, saves, loads the save, lets it
# run again, takes two screenshots, quits. Exercises the script VM, the event system and savegames,
# which is what a port (x64) or a new back end can break without a timedemo noticing.
#
#   tools\smoke-game.ps1 -Platform x64 [-Map mars_city1] [-Extra '+set r_swRenderer 1'] [-Tag x64]
param(
    [string]$Platform = 'x64',
    [string]$Map = 'mars_city1',
    [string]$Extra = '',
    [string]$Tag = 'smoke',
    [int]$RunFrames = 400,
    [int]$Width = 1280,
    [int]$Height = 720
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$savePath = Join-Path $root 'build\save-auto'
$exe = Join-Path $root "build\$Platform\Release\DOOM3.exe"
if (-not (Test-Path (Join-Path $savePath 'base\autoexec.cfg'))) { throw 'run tools\bench-timedemo.ps1 once first: it writes autoexec.cfg' }
$script = @"
map game/$Map
wait $RunFrames
screenshot "screenshots/$Tag-$Map-1.tga"
wait 2
saveGame smoketest
wait 30
loadGame smoketest
wait 200
screenshot "screenshots/$Tag-$Map-2.tga"
wait 2
echo SMOKE_DONE
quit
"@
[System.IO.File]::WriteAllText((Join-Path $savePath 'base\smoke-run.cfg'), $script)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logName = "smoke-$Tag-$stamp.log"
$crashFile = Join-Path (Split-Path $exe) 'crash.txt'
$argLine = "+set fs_basepath `"C:\Program Files (x86)\Steam\steamapps\common\Doom 3`" +set fs_savepath `"$savePath`" " +
    "+set logFile 2 +set logFileName $logName +set r_customWidth $Width +set r_customHeight $Height +set com_skipIntroVideos 1 $Extra +exec smoke-run.cfg"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$tokens = [regex]::Matches($argLine, '"[^"]*"|\S+').Count
if ($tokens -gt 64) { throw "the command line has $tokens tokens; the engine keeps 64 (idCmdArgs::MAX_COMMAND_ARGS) and drops the rest silently" }
$p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path $exe) -PassThru
if (-not $p.WaitForExit(420000)) { $p.Kill(); Write-Output 'TIMED OUT after 420 s: killed' }
$sw.Stop()
if ((Test-Path $crashFile) -and ((Get-Item $crashFile).LastWriteTime -gt (Get-Date).AddSeconds(-$sw.Elapsed.TotalSeconds - 2))) {
    Get-Content $crashFile | Select-Object -First 24
    throw "the engine crashed (exit code $($p.ExitCode))"
}
$log = Get-Content (Join-Path $savePath "base\$logName")
$done = @($log | Where-Object { $_ -match 'SMOKE_DONE' }).Count -gt 0
$errors = @($log | Where-Object { $_ -match '^ERROR:|^\*\*\*\*|Error:' })
$warnings = @($log | Where-Object { $_ -match '^WARNING:' })
Write-Output ("smoke {0} on {1}: done={2}, exit {3}, {4} s, {5} error lines, {6} warnings" -f $Map, $Platform, $done, $p.ExitCode, [int]$sw.Elapsed.TotalSeconds, $errors.Count, $warnings.Count)
if ($warnings.Count -gt 20) { $warnings | Group-Object | Sort-Object Count -Descending | Select-Object -First 3 | ForEach-Object { "   {0}x {1}" -f $_.Count, $_.Name } }
$errors | Select-Object -First 10
