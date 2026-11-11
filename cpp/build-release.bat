@echo off
rem Release build: separate build-release directory with CMAKE_BUILD_TYPE=Release.
rem Usage: cmd /c cpp\build-release.bat [test]
set ROOT=%~dp0..
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo [build] vcvars64 failed & exit /b 1)

if not exist "%~dp0build-release\CMakeCache.txt" cmake -S "%~dp0." -B "%~dp0build-release" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1
cmake --build "%~dp0build-release"
if errorlevel 1 exit /b 1

if "%1"=="test" "%~dp0build-release\pt_tests.exe"
