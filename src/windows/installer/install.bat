@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
if not exist "dirlocker.exe" (
    echo Error: dirlocker.exe not found in current folder.
    pause
    exit /b 1
)
set "INSTALL_DIR=%LOCALAPPDATA%\Programs\DirLocker"
set "TARGET=%INSTALL_DIR%\dirlocker.exe"
if exist "%TARGET%" (
    "%TARGET%" --quit-silent >nul 2>&1
) else (
    "dirlocker.exe" --quit-silent >nul 2>&1
)
timeout /t 1 /nobreak >nul
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
copy /Y "dirlocker.exe" "%TARGET%" >nul
if errorlevel 1 (
    echo 复制程序到 %INSTALL_DIR% 失败，请先退出托盘中运行的旧版本后再试。
    pause
    exit /b 1
)
echo Installing 目录密匣...
"%TARGET%" --install
if errorlevel 1 (
    pause
    exit /b 1
)
start "" "%TARGET%" --show
endlocal
