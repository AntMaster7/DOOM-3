@echo off
rem Launches the DOOM 3 level editor (D3Radiant, built into DOOM3.exe).
rem The editor requires windowed mode; +editor also forces r_fullscreen 0.
setlocal
set BASEPATH=C:\Program Files (x86)\Steam\steamapps\common\Doom 3
set SAVEPATH=%~dp0build\save
if not exist "%SAVEPATH%" mkdir "%SAVEPATH%"
"%~dp0build\Win32\Release\DOOM3.exe" +set fs_basepath "%BASEPATH%" +set fs_savepath "%SAVEPATH%" +set r_fullscreen 0 +set r_mode 5 +set com_allowConsole 1 +editor %*
endlocal
