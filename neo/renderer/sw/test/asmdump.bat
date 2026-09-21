@echo off
rem Assembly listing of the core TU, with the engine's flags: build\sw-test\sw_core.asm
rem   neo\renderer\sw\test\asmdump.bat [extra cl flags]
setlocal
set "ROOT=%~dp0..\..\..\.."
set "OUT=%ROOT%\build\sw-test"
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /c /TC /std:c11 /W3 /O2 /Oi /Ot /fp:fast /arch:AVX512 /FA /Fa"%OUT%\sw_core.asm" /Fo"%OUT%\sw_core_asm.obj" %* "%~dp0..\sw_core.c"
