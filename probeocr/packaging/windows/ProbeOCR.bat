@echo off
rem Starts Probe OCR. Extra arguments are passed through, e.g.  ProbeOCR.bat --root D:\screenshots
if "%~1"=="" (start "" "%~dp0bin\probeocr-gui.exe" --root "%~dp0samples") else (start "" "%~dp0bin\probeocr-gui.exe" %*)
