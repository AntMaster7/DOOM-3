@echo off
rem Launches the x64 build (neo\doom-x64.sln; no editors, those are the Win32 build) against the
rem Steam assets, referenced in place. Saves/configs/logs go to build\save (gitignored).
rem   run-game-x64.bat +set r_swRenderer 1        the CPU renderer
setlocal
set BASEPATH=C:\Program Files (x86)\Steam\steamapps\common\Doom 3
set SAVEPATH=%~dp0build\save
if not exist "%SAVEPATH%" mkdir "%SAVEPATH%"
"%~dp0build\x64\Release\DOOM3.exe" +set fs_basepath "%BASEPATH%" +set fs_savepath "%SAVEPATH%" +set com_allowConsole 1 %*
endlocal
