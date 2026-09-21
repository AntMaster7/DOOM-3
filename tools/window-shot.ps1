# What is REALLY in the DOOM 3 window: its content as the compositor holds it (PrintWindow with
# PW_RENDERFULLCONTENT), not the renderer's own framebuffer. A renderer-side screenshot cannot see
# a presenter that shows the wrong thing (byte order, row order, a stale or black swap chain).
# It reads the WINDOW, never the screen: whatever else the user has open is not captured, and the
# window may be covered.
#   tools\window-shot.ps1 -Out shot.png [-DelaySec 8] [-Scale 0.5]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$DelaySec = 0,
    [double]$Scale = 1.0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class D3Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[void][D3Win]::SetProcessDPIAware()
if ($DelaySec -gt 0) { Start-Sleep -Seconds $DelaySec }
$proc = Get-Process -Name DOOM3* -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw 'no DOOM3 window' }
$r = New-Object D3Win+RECT
[void][D3Win]::GetClientRect($proc.MainWindowHandle, [ref]$r)
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [D3Win]::PrintWindow($proc.MainWindowHandle, $hdc, 3)      # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$g.Dispose()
if (-not $ok) { throw 'PrintWindow failed' }
if ($Scale -ne 1.0) {
    $sw = [int]($w * $Scale); $sh = [int]($h * $Scale)
    $small = New-Object System.Drawing.Bitmap $bmp, $sw, $sh
    $bmp.Dispose(); $bmp = $small
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("window client area {0}x{1} -> {2}" -f $w, $h, $Out)
