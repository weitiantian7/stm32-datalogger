@echo off
setlocal DisableDelayedExpansion
chcp 65001 >nul
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Export-History.ps1" %*
set "history_exit_code=%ERRORLEVEL%"
echo.
pause
exit /b %history_exit_code%
