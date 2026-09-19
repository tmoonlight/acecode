@echo off
REM ACECode TUI development launcher (Windows)
REM Usage: scripts\dev_tui.bat [launcher options]

setlocal
set "SCRIPT_DIR=%~dp0"
call "%SCRIPT_DIR%dev_windows_env.bat"
if errorlevel 1 exit /b %errorlevel%

where python >nul 2>&1
if not errorlevel 1 (
    set "PYTHON=python"
) else (
    where py >nul 2>&1
    if not errorlevel 1 (
        set "PYTHON=py"
    ) else (
        echo [ERROR] python or py was not found. Install Python 3.8+ first.
        exit /b 1
    )
)

"%PYTHON%" "%SCRIPT_DIR%dev_environment.py" tui --yes %*
exit /b %errorlevel%
