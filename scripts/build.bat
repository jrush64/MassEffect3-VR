@echo off
REM Build the MELE3 VR dxgi.dll proxy (x64, static CRT).
REM Output: builds\dxgi.dll
REM Prerequisites: Visual Studio 2022 (Community/Professional/Enterprise or the standalone
REM Build Tools) with the "Desktop development with C++" workload. See BUILD.md.
setlocal
cd /d "%~dp0"

REM Locate any VS2022 install via vswhere (ships with VS2017+ at this fixed path) instead of
REM assuming Community specifically or a fixed drive letter.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" set VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" ( echo [build] vswhere.exe not found - is Visual Studio 2022 installed? & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL ( echo [build] no VS install with MSBuild found & exit /b 1 )

set MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe
if not exist "%MSBUILD%" ( echo [build] MSBuild not found at "%MSBUILD%" & exit /b 1 )

if not exist "%~dp0..\third_party\minhook\src\hook.c" ( echo [build] minhook not found in third_party & exit /b 1 )
if not exist "%~dp0..\third_party\imgui\imgui.cpp" ( echo [build] imgui not found in third_party & exit /b 1 )

"%MSBUILD%" "%~dp0..\src\MELE3VR.vcxproj" /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
if errorlevel 1 ( echo [build] compile/link FAILED & exit /b 1 )

echo [build] OK -^> "%~dp0..\builds\dxgi.dll"
endlocal
