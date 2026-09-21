# Census of one map pose on the GL path: r_frameLog rows, light passes per visible pixel
# (r_showLightCount 4), light passes through walls (3), shadow volume faces per pixel
# (r_showShadowCount 2, with CPU-projected volumes: the tool draws without shadow.vp), a screenshot.
#
#   tools\pose-census.ps1 -Map alphalabs2                          at the spawn point
#   tools\pose-census.ps1 -Map alphalabs2 -Pose '100 -200 64 90'   setviewpos x y z yaw
#
# Output: build\save-auto\base\pose-<map>.csv, the numbers printed below, screenshots\pose-<map>.tga
param(
    [Parameter(Mandatory = $true)][string]$Map,
    [string]$Pose = '',
    [string]$Name = '',
    [int]$Width = 1920,
    [int]$Height = 1080,
    [int]$LoadWaitFrames = 120
)
$ErrorActionPreference = 'Stop'
if ($Name -eq '') { $Name = $Map }
$root = Split-Path -Parent $PSScriptRoot
$savePath = Join-Path $root 'build\save-auto'

# The sequence lives in a .cfg, not in "+" arguments: the engine applies EVERY "+set" of the command
# line at start-up, whatever its position, so a "+set" cannot be sequenced between "+wait"s.
$setPose = ''
if ($Pose -ne '') { $setPose = "noclip`r`nsetviewpos $Pose`r`nwait 30" }
$script = @"
map game/$Map
wait $LoadWaitFrames
notarget
$setPose
screenshot "screenshots/pose-$Name.tga"
wait 2
set r_frameLog "pose-$Name.csv"
wait 30
set r_frameLog ""
echo SECTION lights_visible
set r_showLightCount 4
wait 10
echo SECTION lights_all
set r_showLightCount 3
wait 10
set r_showLightCount 0
echo SECTION shadow_faces
set r_useShadowVertexProgram 0
set r_showShadowCount 2
wait 10
echo SECTION done
quit
"@
[System.IO.File]::WriteAllText((Join-Path $savePath 'base\pose-run.cfg'), $script)
$cmd = '+exec pose-run.cfg'

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logName = "pose-$Name-$stamp.log"
$exe = Join-Path $root 'build\Win32\Release\DOOM3.exe'

# reuse the bench script's autoexec.cfg (written on its every run) so the settings are the same
if (-not (Test-Path (Join-Path $savePath 'base\autoexec.cfg'))) { throw 'run tools\bench-timedemo.ps1 once first: it writes autoexec.cfg' }

$argLine = "+set fs_basepath `"C:\Program Files (x86)\Steam\steamapps\common\Doom 3`" +set fs_savepath `"$savePath`" " +
    "+set logFile 2 +set logFileName $logName +set r_customWidth $Width +set r_customHeight $Height $cmd"
$tokens = [regex]::Matches($argLine, '"[^"]*"|\S+').Count
if ($tokens -gt 64) { throw "the command line has $tokens tokens; the engine keeps 64 (idCmdArgs::MAX_COMMAND_ARGS) and drops the rest silently" }
$p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path $exe) -PassThru
if (-not $p.WaitForExit(180000)) { $p.Kill(); Write-Output 'timed out after 180 s: killed, reading what the log has' }

$log = Get-Content (Join-Path $savePath "base\$logName")
$section = ''
$sums = @{}
foreach ($line in $log) {
    if ($line -match '^SECTION (\S+)') { $section = $Matches[1]; $sums[$section] = New-Object System.Collections.Generic.List[double] }
    elseif ($section -ne '' -and $line -match 'overdraw:\s*([0-9.]+)') { $sums[$section].Add([double]$Matches[1]) }
}
Write-Output "pose $Name ($Map $Pose) ${Width}x${Height}, exit code $($p.ExitCode)"
foreach ($k in 'lights_visible', 'lights_all', 'shadow_faces') {
    if ($sums.ContainsKey($k) -and $sums[$k].Count -gt 0) {
        $last = $sums[$k][$sums[$k].Count - 1]
        Write-Output ("  {0,-16} {1,6:F1} per pixel (last of {2} frames)" -f $k, $last, $sums[$k].Count)
    } else {
        Write-Output "  $k : no data"
    }
}
