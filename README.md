# probeocr

Pull numeric probe readings out of batches of screenshots into a CSV. You draw and label
a box for each probe once, and every screenshot is aligned to a reference so the boxes
follow the window even if it moves.

- [`probeocr/`](probeocr/): the tool itself (C core, Python web frontend, samples,
  smoke test). **Start with [probeocr/README.md](probeocr/README.md).**
- Windows: every push builds a self-contained zip and tests it. Download it from the
  latest run under **Actions → probeocr Windows bundle → Artifacts**.
- `roi_ocr.cpp` / `CMakeLists.txt`: the earlier C++/OpenCV prototype, kept for reference.
