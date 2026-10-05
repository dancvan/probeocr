# probeocr

Pull numeric probe readings out of batches of screenshots into a CSV. You draw and label
a box for each probe once, and every screenshot is aligned to a reference so the boxes
follow the window even if it moves.

- [`probeocr/`](probeocr/): the tool itself (C core, native Clay/raylib app, samples,
  smoke test). **Start with [probeocr/README.md](probeocr/README.md).**
- Windows: every push builds a self-contained zip and tests it. Download it from the
  latest run under **Actions → probeocr Windows bundle → Artifacts → probeocr-win64.zip**.
- `roi_ocr.cpp` / `CMakeLists.txt`: the earlier C++/OpenCV prototype, kept for reference.
- [`stepfit/`](stepfit/): import a step-response CSV, pick the input and output columns, and fit a
  stable zpk model over a chosen frequency range (Python, numpy/scipy; web UI and CLI).
  **Start with [stepfit/README.md](stepfit/README.md).**
