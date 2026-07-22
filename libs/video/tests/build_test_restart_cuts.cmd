@echo off
rem Build + run the growable primitive-restart cut-list regression test.
setlocal
if not defined VCToolsInstallDir call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c17 /W3 /O2 /I..\..\..\include test_restart_cuts.c /Fe:test_restart_cuts.exe
if exist test_restart_cuts.exe "%~dp0test_restart_cuts.exe"
exit /b %errorlevel%
