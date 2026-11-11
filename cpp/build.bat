@echo off
rem Build script: init MSVC x64 env then configure+build with Ninja.
rem Usage: cmd /c cpp\build.bat [test]
set ROOT=%~dp0..
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul
if errorlevel 1 (echo [build] vcvars64 failed & exit /b 1)

if not exist "%~dp0build\CMakeCache.txt" cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1
cmake --build "%~dp0build"
if errorlevel 1 exit /b 1

if "%1"=="test" "%~dp0build\pt_tests.exe"
