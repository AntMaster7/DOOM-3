@echo off
rem Resurrection of Evil on the CPU rasterizer, without OpenGL (r_swRenderer 2): the x64 build keeps the
rem expansion's game DLL in build\x64\Release\d3xp\gamex64.dll and finds it through fs_game, so nothing
rem is swapped (run-roe.bat, the Win32 way, swaps DLLs). Saves/configs/logs go to build\save\d3xp.
setlocal
set BASEPATH=C:\Program Files (x86)\Steam\steamapps\common\Doom 3
set SAVEPATH=%~dp0build\save
if not exist "%SAVEPATH%" mkdir "%SAVEPATH%"
"%~dp0build\x64\Release\DOOM3.exe" +set fs_basepath "%BASEPATH%" +set fs_savepath "%SAVEPATH%" +set com_allowConsole 1 +set fs_game d3xp +set r_swRenderer 2 %*
endlocal
