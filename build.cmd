@echo off
setlocal
call "%~dp0qt-env.cmd"
if errorlevel 1 exit /b 1
if not exist "%~dp0build" mkdir "%~dp0build"
if not exist "%~dp0build" exit /b 1
pushd "%~dp0build"
if errorlevel 1 exit /b 1
"%LANCHAT_QT_DIR%\bin\qmake.exe" ..\LanChat.pro -spec win32-g++ CONFIG+=release CONFIG-=debug
if errorlevel 1 (
    popd
    exit /b 1
)
"%LANCHAT_MINGW_DIR%\bin\mingw32-make.exe" -j2 release
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%