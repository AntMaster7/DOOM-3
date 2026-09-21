@echo off
rem DOOM 3 on the CPU rasterizer, without OpenGL (r_swRenderer 2): x64 build, AVX-512, the frame
rem presented through D3D12. Steam assets referenced in place; saves/configs/logs go to build\save
rem (gitignored), the same place run-game-x64.bat uses.
rem   run-game-sw.bat                                     windowed, the config's r_mode
rem   run-game-sw.bat +set r_fullscreen 1 +set r_mode -1 +set r_customWidth 3840 +set r_customHeight 2160
rem Sets no archived cvar: r_swRenderer is never written to the config, and this mode ignores the
rem texture compression settings (no .dds, no compressed formats), so the config stays as it is.
setlocal
set BASEPATH=C:\Program Files (x86)\Steam\steamapps\common\Doom 3
set SAVEPATH=%~dp0build\save
if not exist "%SAVEPATH%" mkdir "%SAVEPATH%"
"%~dp0build\x64\Release\DOOM3.exe" +set fs_basepath "%BASEPATH%" +set fs_savepath "%SAVEPATH%" +set com_allowConsole 1 +set r_swRenderer 2 %*
endlocal
