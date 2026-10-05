#!/usr/bin/env bash
# Build a self-contained Windows folder + zip: dist/probeocr-win64[.zip]
#
# Run inside an MSYS2 UCRT64 shell with these packages installed:
#   pacman -S make zip mingw-w64-ucrt-x86_64-{gcc,pkgconf,tesseract-ocr,leptonica,raylib}
#
# The result needs nothing installed on the target PC: it carries the native
# app (probeocr-gui.exe), the CLI (probeocr.exe), their DLLs and the OCR model.
set -euo pipefail

here=$(cd "$(dirname "$0")/../.." && pwd)          # the probeocr/ folder
out="$here/dist/probeocr-win64"

rm -rf "$out" "$out.zip"
mkdir -p "$out/bin" "$out/tessdata"

make -C "$here"
cp "$here/bin/probeocr.exe" "$here/bin/probeocr-gui.exe" "$out/bin/"

# Every MSYS2 DLL either exe links against (Windows system DLLs are skipped).
ldd "$out/bin/probeocr.exe" "$out/bin/probeocr-gui.exe" | awk '$3 ~ /^\/ucrt64\// {print $3}' | sort -u \
    | xargs -r cp -t "$out/bin/"

# Same model family the macOS build uses (Homebrew ships tessdata_fast).
curl -fsSL -o "$out/tessdata/eng.traineddata" \
    https://github.com/tesseract-ocr/tessdata_fast/raw/main/eng.traineddata

cp -r "$here/samples" "$here/tools" "$here/README.md" "$out/"
cp "$here/packaging/windows/ProbeOCR.bat" "$here/packaging/windows/README-WINDOWS.txt" "$out/"
find "$out" -name __pycache__ -prune -exec rm -rf {} +

(cd "$here/dist" && zip -qr probeocr-win64.zip probeocr-win64)
echo "bundle: $out.zip ($(du -h "$out.zip" | cut -f1))"
