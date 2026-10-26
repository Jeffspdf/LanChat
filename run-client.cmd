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
title LanChat Client
"%APP%" %*
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
    echo Client exited with error code %RESULT%.
    pause
)
exit /b %RESULT%