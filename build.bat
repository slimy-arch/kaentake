@echo off
setlocal

rem Builds Kaentake.dll (injector) and Kaentake.exe (launcher).
rem Usage: build.bat [Release|Debug]   (default: Release)

cd /d "%~dp0"

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release

if /i "%CONFIG%"=="Release" (
    set PRESET=release-win32
) else if /i "%CONFIG%"=="Debug" (
    set PRESET=debug-win32
) else (
    echo Unknown configuration "%CONFIG%". Use Release or Debug.
    exit /b 1
)

where cmake >nul 2>&1
if errorlevel 1 (
    echo cmake was not found on PATH.
    exit /b 1
)

if not exist "external\Detours\src\detours.cpp" goto submodules
if not exist "external\WzLib\CMakeLists.txt" goto submodules
goto configure

:submodules
echo Initializing submodules...
git submodule update --init --recursive
if errorlevel 1 exit /b 1

:configure
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

cmake --build --preset %PRESET% --target injector
if errorlevel 1 exit /b 1

cmake --build --preset %PRESET% --target launcher
if errorlevel 1 exit /b 1

echo.
echo Built build\%CONFIG%\Kaentake.dll and Kaentake.exe
endlocal
