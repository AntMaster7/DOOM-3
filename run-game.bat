@echo off
rem Launches the VS2026-built DOOM 3 against the Steam assets (referenced in
rem place, never copied). Saves/configs/logs go to build\save (gitignored).
setlocal
set BASEPATH=C:\Program Files (x86)\Steam\steamapps\common\Doom 3
set SAVEPATH=%~dp0build\save
if not exist "%SAVEPATH%" mkdir "%SAVEPATH%"
"%~dp0build\Win32\Release\DOOM3.exe" +set fs_basepath "%BASEPATH%" +set fs_savepath "%SAVEPATH%" +set com_allowConsole 1 %*
endlocal
