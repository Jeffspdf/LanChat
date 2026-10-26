@echo off
if not defined LANCHAT_QT_DIR set "LANCHAT_QT_DIR=C:\Qt\6.8.3\mingw_64"
if not defined LANCHAT_MINGW_DIR set "LANCHAT_MINGW_DIR=C:\Qt\Tools\mingw1310_64"
if not exist "%LANCHAT_QT_DIR%\bin\qmake.exe" (
    echo ERROR: Set LANCHAT_QT_DIR to your Qt MinGW kit directory.
    exit /b 1
)
if not exist "%LANCHAT_MINGW_DIR%\bin\mingw32-make.exe" (
    echo ERROR: Set LANCHAT_MINGW_DIR to your matching MinGW directory.
    exit /b 1
)
set "PATH=%LANCHAT_QT_DIR%\bin;%LANCHAT_MINGW_DIR%\bin;%PATH%"
set "QT_PLUGIN_PATH=%LANCHAT_QT_DIR%\plugins"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%LANCHAT_QT_DIR%\plugins\platforms"
set "QT_QPA_PLATFORM=windows"
exit /b 0