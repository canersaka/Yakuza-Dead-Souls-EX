@echo off
rem s30: pre-DIVSQ-fix baseline build -- separate exe name to avoid file locks.
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
)
cl /nologo /std:c17 /W3 /O2 /DNDEBUG /I..\..\..\include ^
   replay_main.c ..\rsx_dispatch.c ..\rsx_fp_decompiler.c ..\rsx_vp_decompiler.c ^
   /Foobj_before\ /Fe:replay_before.exe /link d3d12.lib dxgi.lib d3dcompiler.lib
exit /b %errorlevel%
