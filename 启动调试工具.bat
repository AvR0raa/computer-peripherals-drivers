@echo off
rem 外设调试工具启动器 — 双击运行
cd /d "%~dp0"
if exist ".venv\Scripts\pythonw.exe" (
    start "" ".venv\Scripts\pythonw.exe" gui.py
) else (
    echo [错误] 找不到虚拟环境，请先运行:
    echo   python -m venv .venv
    echo   .venv\Scripts\pip install -r requirements.txt
    pause
)
