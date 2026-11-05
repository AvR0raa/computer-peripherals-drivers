@echo off
rem M6.1 打包脚本 — 产出 dist\PeripheralTool\PeripheralTool.exe（绿色目录版）
cd /d "%~dp0.."
.venv\Scripts\python -m PyInstaller --noconfirm --clean --windowed ^
    --name PeripheralTool ^
    --add-data "protocol_db;protocol_db" ^
    gui.py
echo.
echo 输出: dist\PeripheralTool\PeripheralTool.exe
echo 整个 dist\PeripheralTool 文件夹可拷贝到其他 Windows 机器运行（无需装 Python）。
pause
