@echo off
rem Starts the Probe OCR frontend and opens it in your browser.
rem Any extra arguments are passed through, e.g.  ProbeOCR.bat --root D:\screenshots
setlocal
cd /d "%~dp0"
"%~dp0python\python.exe" "%~dp0frontend\server.py" --root "%~dp0." %*
if errorlevel 1 pause
