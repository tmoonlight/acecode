@echo off
REM Initialize the Visual Studio x64 C++ developer environment for dev launchers.

setlocal EnableDelayedExpansion
set "ACECODE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!ACECODE_VSWHERE!" goto :missing_installer

set "ACECODE_VSINSTALL="
for /f "usebackq delims=" %%I in (`"!ACECODE_VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ACECODE_VSINSTALL=%%I"
if not defined ACECODE_VSINSTALL goto :missing_tools

set "ACECODE_VSDEVCMD=!ACECODE_VSINSTALL!\Common7\Tools\VsDevCmd.bat"
if not exist "!ACECODE_VSDEVCMD!" goto :missing_command

for %%I in ("!ACECODE_VSDEVCMD!") do endlocal & set "ACECODE_VSDEVCMD=%%~fI"
call "%ACECODE_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 goto :initialization_failed
exit /b 0

:missing_installer
endlocal
echo [ERROR] Visual Studio Installer was not found. Install Visual Studio Build Tools with the Desktop development with C++ workload.
exit /b 1

:missing_tools
endlocal
echo [ERROR] Visual Studio C++ tools were not found. Install the Desktop development with C++ workload.
exit /b 1

:missing_command
endlocal
echo [ERROR] Visual Studio developer command script was not found.
exit /b 1

:initialization_failed
echo [ERROR] Failed to initialize the Visual Studio C++ developer environment.
exit /b 1
