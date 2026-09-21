# Writes the fixed set of demo1 frames through com_demoShotFrames (the capture path both back ends
# share) into build\save-auto\base\demoshots\<demo>_<tag>_<w>x<h>_f<frame>.tga.
#
#   tools\capture-reference.ps1                 GL reference, matched filtering (plan 6.10)
#   tools\capture-reference.ps1 -Trilinear      GL with the engine's default trilinear filter
#   tools\capture-reference.ps1 -Tag sw -Extra '+set r_swRenderer 1'
#
# The frame list comes from the Stage 0 census (CLAUDE.md): spread over the demo plus the extremes
# of each counter.
param(
    [string]$Tag = 'gl-matched',
    [string]$Frames = '100 223 400 657 743 1093 1181 1204 1500 1891 2100',
    [string]$Extra = '',
    [string]$Platform = 'Win32',
    [switch]$Trilinear,
    [int]$Width = 1920,
    [int]$Height = 1080
)
$filter = '+set image_filter GL_LINEAR_MIPMAP_NEAREST'
if ($Trilinear) {
    $filter = '+set image_filter GL_LINEAR_MIPMAP_LINEAR'
    if ($Tag -eq 'gl-matched') { $Tag = 'gl-trilinear' }
}
# s_constantAmplitude 1: light materials that follow sound amplitude sample it by wall-clock time, so
# the same demo frame lands on a different point of the flicker in every run (20 dB between two GL
# runs at frame 1181). Captures pin it; benches do not.
& (Join-Path $PSScriptRoot 'bench-timedemo.ps1') -Label "capture-$Tag" -Platform $Platform -Width $Width -Height $Height `
    -Extra "+set com_demoShotFrames `"$Frames`" +set com_demoShotTag $Tag +set s_constantAmplitude 1 $filter $Extra"
Get-ChildItem (Join-Path (Split-Path -Parent $PSScriptRoot) "build\save-auto\base\demoshots\*_${Tag}_*") |
    Select-Object Name, Length
