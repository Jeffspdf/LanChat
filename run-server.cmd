@echo off
setlocal
call "%~dp0qt-env.cmd"
if errorlevel 1 (
    pause
    exit /b 1
)
set "APP=%~dp0build\release\LanChat.exe"
if not exist "%APP%" (
    echo ERROR: Run build.cmd or build Release into the build directory first.
    pause
    exit /b 1
)
cd /d "%~dp0"
title LanChat Server - keep this window open
"%APP%" --server --port 45454 %*
set "RESULT=%ERRORLEVEL%"
echo Server stopped. Exit code: %RESULT%
pause
exit /b %RESULT%