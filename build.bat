@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found; install Visual Studio or the Build Tools with the C++ workload & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio install with the C++ x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /EHsc /MT /W4 /std:c++17 /DUNICODE /D_UNICODE hdr_screenshot_fix.cpp /Fe:hdr_screenshot_fix.exe /link windowscodecs.lib ole32.lib || exit /b 1
del /q hdr_screenshot_fix.obj 2>nul
echo Built hdr_screenshot_fix.exe
