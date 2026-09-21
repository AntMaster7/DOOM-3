# The fixed demo1 frame set through BOTH back ends in one process (swCompareShot per frame):
# prints PSNR / largest difference / differing share per frame and leaves
# build\save-auto\base\demoshots\demo1_cmp_f<frame>_{gl,sw}.tga for tools\triptych.py.
#   tools\sw-compare-demo.ps1 [-Frames '100 223 ..'] [-Extra '+set r_skipPostProcess 1'] [-Width 1920 -Height 1080]
param(
    [string]$Frames = '100 223 400 657 743 1093 1181 1204 1500 1891 2100',
    [string]$Extra = '',
    [int]$Width = 1920,
    [int]$Height = 1080,
    [string]$Demo = 'demo1'
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'sw-run.ps1') -Tag 'cmpdemo' -Sw -NoQuit -Width $Width -Height $Height -TimeoutSec 600 `
    -Extra ("+set com_demoShotCompare 1 +set com_demoShotFrames `"$Frames`" " + $Extra) `
    -Script @("timeDemoQuit $Demo") -Show 'swcompare|demo frame'
