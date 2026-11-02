@echo off
rem Builds hod2fix\ddraw.dll (32-bit) with Visual Studio 2022.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist %VCVARS% (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VCVARS="%%i\VC\Auxiliary\Build\vcvars32.bat"
)
call %VCVARS% >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /EHsc /W3 /GS- /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Fdbuild\ ^
   src\main.cpp src\ddraw.cpp src\d3d7.cpp src\renderer.cpp src\input.cpp src\audio.cpp ^
   /LD /Fe:build\ddraw.dll /link /DEF:src\ddraw.def /NOLOGO /SAFESEH:NO ^
   user32.lib gdi32.lib winmm.lib dxguid.lib || exit /b 1
echo Built build\ddraw.dll
