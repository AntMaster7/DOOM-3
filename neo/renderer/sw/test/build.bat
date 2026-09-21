@echo off
rem Builds the standalone x64 harness of the software rasterizer core into build\sw-test\.
rem   neo\renderer\sw\test\build.bat [extra cl flags for the core, e.g. /fp:precise]
rem The core TU gets exactly the flags the engine project will give it (plan 6.2).
setlocal
set "ROOT=%~dp0..\..\..\.."
set "OUT=%ROOT%\build\sw-test"
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo BUILD FAILED: vcvars64 & exit /b 1 )
rem /d2SSAOptimizer-: without it the optimizer dies in raster_tri (C1001, p2 main.cpp line 262, toolset 14.51) as soon
rem as the core grows past some size; the kernel benches are the same with and without it (2026-09-21).
cl /nologo /c /TC /std:c11 /W3 /O2 /Oi /Ot /fp:fast /arch:AVX512 /d2SSAOptimizer- /Zi /Fo"%OUT%\sw_core.obj" /Fd"%OUT%\sw_test.pdb" %* "%~dp0..\sw_core.c"
if errorlevel 1 ( echo BUILD FAILED: sw_core.c & exit /b 1 )
cl /nologo /c /TC /std:c11 /W3 /O2 /Zi /Fo"%OUT%\sw_test.obj" /Fd"%OUT%\sw_test.pdb" "%~dp0sw_test.c"
if errorlevel 1 ( echo BUILD FAILED: sw_test.c & exit /b 1 )
link /nologo /DEBUG /OUT:"%OUT%\sw_test.exe" "%OUT%\sw_core.obj" "%OUT%\sw_test.obj"
if errorlevel 1 ( echo BUILD FAILED: link & exit /b 1 )
echo built %OUT%\sw_test.exe
