@echo off
rem Build the Track B capture-replay harness (MSVC).
rem Run from this directory: build_replay.cmd
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
)
cl /nologo /std:c17 /W3 /O2 /DNDEBUG /I..\..\..\include ^
   replay_main.c ..\rsx_dispatch.c ^
   /Fe:replay.exe /link d3d12.lib dxgi.lib d3dcompiler.lib
exit /b %errorlevel%
