@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 goto try_python
py -3 tools\ui_demo.py
goto finished
:try_python
where python >nul 2>nul
if errorlevel 1 goto missing
python tools\ui_demo.py
:finished
if errorlevel 1 (
  echo.
  echo The demo could not start. See the message above and the setup guide in README.md.
  pause
)
exit /b
:missing
echo Install Python 3.12 or newer, then open Start Demo again.
pause
