@echo off
chcp 65001 >nul
setlocal
set "INSTALLED=%LOCALAPPDATA%\Programs\DirLocker\dirlocker.exe"
if exist "%INSTALLED%" (
    "%INSTALLED%" --uninstall
) else if exist "%~dp0dirlocker.exe" (
    "%~dp0dirlocker.exe" --uninstall
) else (
    echo Error: dirlocker.exe not found.
    pause
    exit /b 1
)
endlocal

