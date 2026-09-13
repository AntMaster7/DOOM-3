@echo off
rem Launches the Resurrection of Evil expansion (d3xp). The engine always
rem loads gamex86.dll from the executable's directory, so this swaps in the
rem d3xp game DLL (stashed at build\Win32\Release\d3xp\gamex86.dll by the
rem Game-d3xp build) for the duration of the run and restores the base game
rem DLL when the game exits.
setlocal
set BASEPATH=C:\Program Files (x86)\Steam\steamapps\common\Doom 3
set SAVEPATH=%~dp0build\save
set OUTDIR=%~dp0build\Win32\Release
if not exist "%SAVEPATH%" mkdir "%SAVEPATH%"
if not exist "%OUTDIR%\d3xp\gamex86.dll" (
    echo d3xp game DLL not found - build the Game-d3xp project first.
    exit /b 1
)
rem Keep the existing backup if a previous run was interrupted, so we never
rem overwrite the saved base DLL with the d3xp one.
if not exist "%OUTDIR%\gamex86.dll.base-backup" copy /y "%OUTDIR%\gamex86.dll" "%OUTDIR%\gamex86.dll.base-backup" >nul
copy /y "%OUTDIR%\d3xp\gamex86.dll" "%OUTDIR%\gamex86.dll" >nul
"%OUTDIR%\DOOM3.exe" +set fs_basepath "%BASEPATH%" +set fs_savepath "%SAVEPATH%" +set fs_game d3xp +set com_allowConsole 1 %*
copy /y "%OUTDIR%\gamex86.dll.base-backup" "%OUTDIR%\gamex86.dll" >nul
del "%OUTDIR%\gamex86.dll.base-backup" >nul
endlocal
