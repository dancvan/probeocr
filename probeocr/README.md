# probeocr

Pull probe readings out of a batch of screenshots into a CSV.

- **C core** (`src/probeocr.c`) uses Tesseract and Leptonica. It aligns each screenshot to a reference, then OCRs each labelled region.
- **Python frontend** (`frontend/`) uses only the standard library and opens in your browser. You draw a box around each probe's reading, type its column header, run the batch, fix any flagged cells, and export CSV.

## Setup

```bash
brew install tesseract leptonica pkg-config
make
```

If `make` complains about the Xcode license, either run `sudo xcodebuild -license`
or build with the Command Line Tools:
`DEVELOPER_DIR=/Library/Developer/CommandLineTools make`.

## Frontend

```bash
python3 frontend/server.py                  # tree of the current directory
python3 frontend/server.py --root ~/runs    # or any other folder
```

1. **Pick images**: the sidebar shows every folder under the root that contains images.
   Tick a folder to select everything under it, then clear individual screenshots as needed.
   Only ticked images are processed and exported. You can also type a different root and
   click **Open**. The first selected image becomes the reference (**REF**); click any image
   and use *Make this the reference* to change it.
2. **Probe boxes**: drag a box around each reading and type its header. Use *Number* mode for numeric readouts and *Text* for anything else.
3. **Anchor** (recommended): drag around something that appears in every screenshot, such as a window title or a fixed label. Each screenshot is searched ±N px around that spot, and every probe box moves by the offset found. This keeps things working when the window doesn't sit in exactly the same place each time.
4. **Test this image**, then **Run batch**. Cells with low OCR confidence or no value are highlighted. Click one to jump to that screenshot, then type the correct value.
5. **Export CSV**. **Save layout** reuses the boxes and headers for later batches.

Hidden folders, `node_modules`, `__pycache__` and virtualenvs are skipped; the tree shows at most 5000 images.

**Zoom & pan** (for large screenshots): ⌘/Ctrl + scroll or trackpad pinch zooms around the
cursor (5%–3200%), and the − / % / + / Fit buttons in the toolbar do the same. Scroll,
Space + drag, or middle-button drag pans. Above 200%, pixels are drawn as sharp squares so
box edges can be placed exactly. Zoom and position are kept when switching between
screenshots of the same size, so you can step through a batch looking at the same probe.

Keys: `[` / `]` switch images, `+` / `-` zoom, `0` fit, `1` actual pixels, arrows nudge the
selected box (Shift = 10 px), ⌫ deletes it.

## CLI

```bash
bin/probeocr --csv out.csv layout.txt shots/*.png
ls shots/*.png | bin/probeocr layout.txt -        # paths on stdin
```

Layout format (the frontend writes this for you):

```
ref    /path/to/reference.png        # needed only with an anchor
anchor <x> <y> <w> <h> <search_px>
probe  <x> <y> <w> <h> num|text <column header, spaces allowed>
```

stdout gets one JSON object per image: `dx`, `dy`, `anchor_score`, and for each probe
`value`, `raw` and `conf`. `--debug DIR` saves the preprocessed crops that
Tesseract sees, which helps when tuning box sizes.

## Sample data

`python3 tools/make_samples.py samples 12` generates jittered fake screenshots plus a
`truth.csv` (needs ImageMagick).

## Windows

`packaging/windows/bundle.sh` builds `dist/probeocr-win64.zip`. The zip contains
`probeocr.exe` with its DLLs, the English OCR model, an embeddable Python, the frontend,
the samples, and `ProbeOCR.bat`. The target PC needs nothing else installed.

- **Automatically:** push to GitHub. `.github/workflows/probeocr-windows.yml` builds the
  bundle on a Windows runner, then runs the smoke test outside MSYS2, which catches any DLL
  missing from the bundle. The zip is attached to the run as an artifact.
- **By hand on a Windows PC:** install [MSYS2](https://www.msys2.org), open the
  *UCRT64* shell, and run:
  ```bash
  pacman -S make zip unzip mingw-w64-ucrt-x86_64-{gcc,pkgconf,tesseract-ocr,leptonica}
  bash packaging/windows/bundle.sh
  ```

## Checking an install

```bash
python3 tools/smoke_test.py            # dev checkout
python\python.exe tools\smoke_test.py .   # inside an unzipped Windows bundle
```

This runs all 12 samples through both the CLI and the web server. The samples are copied
into a folder whose name has spaces, `°` and `é`, to exercise path handling. Every reading
is compared with `samples/truth.csv`, and the test prints `PASS` only if all 36 match.
