@echo off
rem Build and run the standalone fragment-decompiler and alpha-test checks.
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
)
cl /nologo /std:c17 /W4 /I..\..\..\include ^
   test_fp_decompiler.c ..\rsx_fp_decompiler.c ^
   /Fe:test_fp.exe || exit /b 1
test_fp.exe
exit /b %errorlevel%
