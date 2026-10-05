# probeocr

Pull probe readings out of a batch of screenshots into a CSV.

- **C core** (`src/core.c`) uses Tesseract and Leptonica. It aligns each screenshot to a
  reference, then OCRs each labelled region. The CLI (`src/probeocr.c`) and the app both use it.
- **Native app** (`src/gui/`) is a single C program with its own window. It uses
  [Clay](https://github.com/nicbarker/clay) for layout and [raylib](https://www.raylib.com)
  for drawing, with no Python or browser involved. You draw a box around each probe's
  reading, type its column header, run the batch, fix any flagged cells, and export CSV.

> This is the `clay-ui` branch. The `main` branch has the same tool with a Python + browser frontend.

## Setup (macOS)

```bash
brew install tesseract leptonica raylib pkg-config
make            # builds bin/probeocr (CLI) and bin/probeocr-gui (app)
```

If `make` complains about the Xcode license, either run `sudo xcodebuild -license`
or build with the Command Line Tools:
`DEVELOPER_DIR=/Library/Developer/CommandLineTools make`.

## The app

```bash
bin/probeocr-gui                    # tree of the current directory
bin/probeocr-gui --root ~/runs      # or any other folder
```

1. **Pick images**: the sidebar shows every folder under the root that contains images.
   Tick a folder to select everything under it, then clear individual screenshots as needed.
   Only ticked images are processed and exported. To switch folders, type a path and click
   **Open**, or drag a folder onto the window. The first selected image becomes the
   reference (**REF**); click any image and use *Make this the reference* to change it.
2. **Probe boxes**: drag a box around each reading, then type its header straight away. Use
   *Number* mode for numeric readouts and *Text* for anything else.
3. **Anchor** (recommended): click **+ Anchor** and drag around something that appears in
   every screenshot, such as a window title or a fixed label. Each screenshot is searched
   ±N px around that spot, and every probe box moves by the offset found. This keeps things
   working when the window doesn't sit in exactly the same place each time.
4. **Test this image**, then **Run batch**. OCR runs on up to 4 background threads, so the
   window stays responsive and **Stop** works mid-batch. Cells with low OCR confidence or no
   value are highlighted. Click one to edit it (the view jumps to that screenshot), then
   press Enter.
5. **Export CSV** writes `<layout name>.csv` (or `probe_readings.csv`) into the root folder.
   **Save layout** keeps the boxes, headers and anchor for later batches, in `layouts/`.

Hidden folders, `node_modules`, `__pycache__` and virtualenvs are skipped; the tree shows at
most 5000 images.

**Zoom & pan** (for large screenshots): ⌘/Ctrl + scroll zooms around the cursor (5%–3200%),
and the − / % / + / Fit buttons in the toolbar do the same. Scroll, Space + drag, or
middle-button drag pans. Above 200%, pixels are drawn as sharp squares so box edges can be
placed exactly. Zoom and position are kept when switching between screenshots of the same
size, so you can step through a batch looking at the same probe.

Keys: `[` / `]` switch images, `+` / `-` zoom, `0` fit, `1` actual pixels, arrows nudge the
selected box (Shift = 10 px), ⌫ deletes it, ⌘/Ctrl+S saves the layout.

## CLI

```bash
bin/probeocr --csv out.csv layout.txt shots/*.png
ls shots/*.png | bin/probeocr layout.txt -        # paths on stdin
```

Layout format (the app's **Save layout** writes these to `layouts/`):

```
root   /folder/the/screenshots/live/in   # optional, used by the app
ref    /path/to/reference.png            # needed only with an anchor
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
`probeocr-gui.exe` and `probeocr.exe` with their DLLs, the English OCR model, the samples,
and `ProbeOCR.bat`. The target PC needs nothing else installed.

- **Automatically:** push to GitHub. `.github/workflows/probeocr-windows.yml` builds the
  bundle on a Windows runner, then checks it outside MSYS2, which catches any missing DLL:
  - the smoke test runs the CLI on the samples, and starts the app;
  - the app's self-test (below) then runs on Mesa's software OpenGL, since the runner has no
    GPU. Mesa is only used for this test, not shipped in the bundle.

  The zip is attached to the run as an artifact.
- **By hand on a Windows PC:** install [MSYS2](https://www.msys2.org), open the
  *UCRT64* shell, and run:
  ```bash
  pacman -S make zip mingw-w64-ucrt-x86_64-{gcc,pkgconf,tesseract-ocr,leptonica,raylib}
  bash packaging/windows/bundle.sh
  ```

## Checking an install

```bash
python3 tools/smoke_test.py          # CLI on all samples + the app starts
python3 tools/smoke_test.py --gui    # ... plus the app's interactive self-test
```

The samples are copied into a folder whose name has spaces, `°` and `é`, to exercise path
handling, and every reading is compared with `samples/truth.csv`.

`--gui` runs `probeocr-gui --selftest`, which opens the app on those samples and runs the
batch through its worker threads. It then drives the real UI with injected mouse, wheel and
keyboard events:
- clicking tree rows and checkboxes,
- editing a table cell,
- exporting CSV,
- zooming and panning,
- drawing a new box, typing its header and switching it to Text mode,
- "Test this image",
- deleting a box,
- saving and reloading a layout.

It checks every step and saves a screenshot.
