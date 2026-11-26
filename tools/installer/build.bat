@echo off
rem Build SCDA_Setup.exe. Needs runtime/build/scda.exe (tools/build_runtime.bat) and tools/xex_dump/xex_dump.exe (tools/xex_dump/build.bat) first.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d C:\SCDA-Recomp\tools\installer
rem Version shown by the update prompt and stored in HKCU\Software\SCDARecomp: the git short hash.
set SCDA_VER=dev
for /f %%v in ('git rev-parse --short HEAD') do set SCDA_VER=%%v
rc /nologo /fo installer.res installer.rc || exit /b 1
clang-cl /nologo /O2 /DSCDA_VERSION=\"%SCDA_VER%\" /std:c++20 /EHsc /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS installer.cpp installer.res /Fe:SCDA_Setup.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib bcrypt.lib windowscodecs.lib msimg32.lib uxtheme.lib advapi32.lib || exit /b 1
rem RecompLauncher: the portable zip, SCDA_Setup.exe with recomplauncher.json next to it.
set SCDA_NUM=0.0.0
for /f "tokens=3" %%n in ('findstr /b /c:"project(SCDARecomp VERSION" ..\..\runtime\CMakeLists.txt') do set SCDA_NUM=%%n
copy /y ..\..\recomplauncher.json recomplauncher.json >nul || exit /b 1
del /q SCDA-Recomp-%SCDA_NUM%-Windows-x64.zip 2>nul
tar -a -c -f SCDA-Recomp-%SCDA_NUM%-Windows-x64.zip SCDA_Setup.exe recomplauncher.json || exit /b 1
del /q recomplauncher.json
echo Built SCDA-Recomp-%SCDA_NUM%-Windows-x64.zip
