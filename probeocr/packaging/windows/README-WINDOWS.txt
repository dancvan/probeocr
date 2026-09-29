Probe OCR for Windows
=====================

Nothing to install. Unzip this folder anywhere and double-click ProbeOCR.bat.
Your browser opens the app; keep the black console window open while you use it
(close it to stop). The sample screenshots in "samples" are ready to try.

To work on your own screenshots, type their folder (e.g. D:\runs\2026-09) into
the box at the top and click Open, or start it with:
    ProbeOCR.bat --root D:\runs

First launch: Windows SmartScreen may warn about an unrecognised app because the
files are not code-signed. Click "More info" then "Run anyway".

Self-check (should print PASS):
    python\python.exe tools\smoke_test.py .
