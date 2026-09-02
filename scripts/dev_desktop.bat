@echo off
REM ACECode Desktop 一键开发脚本 (Windows)
REM 用法: scripts\dev_desktop.bat [选项]
REM 详见 python scripts\dev_desktop.py --help

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"

REM 选择 python 解释器
where python >nul 2>&1
if %errorlevel%==0 (
    set "PYTHON=python"
) else (
    where py >nul 2>&1
    if %errorlevel%==0 (
        set "PYTHON=py"
    ) else (
        echo [ERROR] 未找到 python 或 py，请先安装 Python 3.8+
        exit /b 1
    )
)

"%PYTHON%" "%SCRIPT_DIR%dev_desktop.py" %*
exit /b %errorlevel%
