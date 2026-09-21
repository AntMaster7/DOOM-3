# The software renderer at the poses the user marked with `markPose` (poses.txt, one line per pose:
#   game/alphalabs2 | x y z yaw | pitch p | 3840x2160 | note ).
# Per pose: the map is loaded, the view is put there (noclip, notarget), 300 frames are rendered, and
# the rasterizer's own report over them is printed (period = the frame as the player gets it) with
# a screenshot in build\save-auto\<game>\screenshots\posebench_<n>.tga.
#
#   tools\pose-bench.ps1 -PoseFile build\save\base\poses.txt [-Width 3840 -Height 2160] [-Extra '+set r_swRenderScale 0.75']
#
# A pose is a still view: it says where the time goes THERE, not what playing through it feels like
# (CLAUDE.md, rule 4). Copy the file out of build\save first if it should not be read in place:
# this script only reads it.
param(
    [Parameter(Mandatory = $true)][string]$PoseFile,
    [int]$Width = 3840,
    [int]$Height = 2160,
    [int]$Frames = 300,
    [string]$Extra = ''
)
$ErrorActionPreference = 'Stop'
$n = 0
foreach ($line in Get-Content $PoseFile) {
    $parts = $line.Split('|') | ForEach-Object { $_.Trim() }
    if ($parts.Count -lt 2 -or $parts[0] -eq '') { continue }
    $n++
    $map = $parts[0]
    $pose = $parts[1]
    $note = if ($parts.Count -gt 4) { $parts[4] } else { '' }
    Write-Output ("=== pose {0}: {1} at {2}  {3}" -f $n, $map, $pose, $note)
    $script = @("map $map", 'wait 150', 'notarget', 'noclip', "setviewpos $pose", 'wait 60', "wait $Frames", "swReportStats $Frames", "screenshot posebench_$n.tga", 'wait 5')
    & (Join-Path $PSScriptRoot 'sw-run.ps1') -Tag "posebench$n" -Width $Width -Height $Height -TimeoutSec 300 `
        -Extra ("+set r_swRenderer 2 +set r_swProfile 1 " + $Extra) -Script $script `
        -Show 'swstats (period|total|setup|tiles|t\.shadow|t\.light|t\.colour|MpxStenc|MpxLtAsk|MpxColor) ' |
        Select-String -NotMatch 'Loaded pk4'
}
if ($n -eq 0) { Write-Output "no poses in $PoseFile" }
