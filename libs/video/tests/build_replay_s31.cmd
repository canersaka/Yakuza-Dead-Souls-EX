@echo off
rem s31: pass-boundary depth tracking + logical-size surfaces -- separate exe name to avoid file locks.
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
)
if not exist obj_s31 mkdir obj_s31
cl /nologo /std:c17 /W3 /O2 /DNDEBUG /I..\..\..\include ^
   replay_main.c ..\rsx_dispatch.c ..\rsx_fp_decompiler.c ..\rsx_vp_decompiler.c ^
   /Foobj_s31\ /Fe:replay_s31.exe /link d3d12.lib dxgi.lib d3dcompiler.lib
exit /b %errorlevel%
