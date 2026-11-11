@echo off
rem Package the C++ dist folder into a release zip (same layout as Python-version zips).
rem Usage: cmd /c cpp\package.bat
set VERSION=0.9.0
powershell -NoProfile -Command "Compress-Archive -Path '%~dp0..\dist\PeripheralTool\*' -DestinationPath '%~dp0..\PeripheralTool-v%VERSION%-cpp-win64.zip' -Force"
if errorlevel 1 exit /b 1
echo [package] PeripheralTool-v%VERSION%-cpp-win64.zip written
