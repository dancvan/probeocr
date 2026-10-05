#!/usr/bin/env python3
"""End-to-end check that a probeocr install reproduces the sample results.

    python3 tools/smoke_test.py [--gui] [INSTALL_DIR]

INSTALL_DIR is a dev checkout (probeocr/) or an unpacked bundle; it defaults to
the folder containing tools/. The samples are copied into a folder whose name
has spaces and non-ASCII characters, then run through the CLI (and the web
server or native GUI, whichever is present), and every reading is compared
with samples/truth.csv. Exits non-zero on any mismatch.

The native GUI is always checked with --version (which also proves its DLLs
load on Windows); --gui additionally runs its interactive self-test, which
needs a display with OpenGL 3.3.
"""
import csv
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

ARGS = [a for a in sys.argv[1:] if a != "--gui"]
RUN_GUI = "--gui" in sys.argv[1:]
ROOT = Path(ARGS[0] if ARGS else Path(__file__).resolve().parent.parent).resolve()
EXT = ".exe" if os.name == "nt" else ""
EXE = ROOT / "bin" / f"probeocr{EXT}"
GUI = ROOT / "bin" / f"probeocr-gui{EXT}"
TESSDATA = ROOT / "tessdata"
LABELS = ["Temperature (°C)", "Pressure, kPa", "Voltage (mV)"]
failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def compare(rows, truth, where):
    """rows: {filename: [v1, v2, v3]}"""
    good = total = 0
    for name, exp in truth.items():
        got = rows.get(name)
        if got is None:
            check(False, f"{where}: no result for {name}")
            continue
        for g, e in zip(got, exp):
            total += 1
            try:
                ok = f"{float(g):.2f}" == e
            except ValueError:
                ok = False
            good += ok
            if not ok:
                print(f"        {name}: got {g!r}, expected {e}")
    check(good == total, f"{where}: {good}/{total} readings correct")


def check_server(work, truth, ref):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    srv = subprocess.Popen([sys.executable, str(ROOT / "frontend" / "server.py"),
                            "--no-browser", "--port", str(port), "--root", str(work.parent)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    base = f"http://127.0.0.1:{port}"
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(base + "/", timeout=1).read()
                break
            except OSError:
                time.sleep(0.2)
        tree = json.load(urllib.request.urlopen(base + "/api/tree"))
        check(tree["count"] == len(truth), f"/api/tree finds {tree['count']} images")
        images = [str(work / n) for n in truth]
        body = json.dumps({"images": images, "layout": {
            "ref": ref, "anchor": {"x": 98, "y": 45, "w": 191, "h": 30, "search": 30},
            "probes": [{"label": l, "x": 211, "y": y, "w": 152, "h": 48, "mode": "num"}
                       for y, l in zip((80, 170, 260), LABELS)]}}).encode()
        req = urllib.request.Request(base + "/api/run", body, {"Content-Type": "application/json"})
        results = [json.loads(line) for line in urllib.request.urlopen(req).read().decode("utf-8").splitlines() if line.strip()]
        fatal = [r["fatal"] for r in results if "fatal" in r]
        check(not fatal, "server run had no fatal error" + (f": {fatal}" if fatal else ""))
        ok = [r for r in results if r.get("ok")]
        if ok:
            check([p["label"] for p in ok[0]["probes"]] == LABELS, "server keeps Unicode labels")
        compare({Path(r["image"]).name: [p["value"] for p in r["probes"]] for r in ok}, truth, "server")
        img = urllib.request.urlopen(base + "/api/image?path=" + urllib.parse.quote(ref)).read()
        check(img[:4] == b"\x89PNG", "/api/image serves a file from a Unicode path")
    finally:
        srv.terminate()
        srv.wait(timeout=5)



def check_gui(work):
    proc = subprocess.run([str(GUI), "--version"], capture_output=True, encoding="utf-8", errors="replace", timeout=60)
    check(proc.returncode == 0 and "probeocr-gui" in proc.stdout,
          f"GUI starts: {proc.stdout.strip() or proc.stderr.strip() or proc.returncode}")
    if not RUN_GUI:
        return
    shot = work.parent / "gui.png"
    proc = subprocess.run([str(GUI), "--selftest", str(work), str(shot)], capture_output=True,
                          encoding="utf-8", errors="replace", timeout=300)
    lines = [l for l in proc.stdout.splitlines() if l.startswith("  ") and ("ok" in l[:8] or "FAIL" in l[:8])]
    for l in lines:
        print("  GUI" + l[1:])
    check(proc.returncode == 0 and "PASS" in proc.stdout,
          f"GUI self-test from a Unicode folder ({len(lines)} checks)" + (f" — {proc.stderr.strip()[-300:]}" if proc.returncode else ""))


def main():
    print(f"probeocr smoke test\n  install: {ROOT}\n  python:  {sys.version.split()[0]} ({sys.platform})")
    check(EXE.is_file(), f"binary present ({EXE.name})")
    if failures:
        return 1

    work = Path(tempfile.mkdtemp()) / "smoke test °C é"
    shutil.copytree(ROOT / "samples", work)
    with open(work / "truth.csv", encoding="utf-8") as f:
        truth = {r["image"]: [r["CH1"], r["CH2"], r["CH3"]] for r in csv.DictReader(f)}
    ref = str(work / "shot_000.png")

    # ---- CLI --------------------------------------------------------
    layout = work / "layout.txt"
    layout.write_text(
        f"ref {ref}\nanchor 98 45 191 30 30\n"
        + "".join(f"probe 211 {y} 152 48 num {l}\n" for y, l in zip((80, 170, 260), LABELS)),
        encoding="utf-8")
    out_csv = work / "out.csv"
    cmd = [str(EXE)] + (["--tessdata", str(TESSDATA)] if TESSDATA.is_dir() else [])
    cmd += ["--csv", str(out_csv), str(layout)] + [str(work / n) for n in truth]
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    check(proc.returncode == 0, f"CLI exit code 0 ({time.time() - t0:.1f}s)"
          + (f" — stderr: {proc.stderr.strip()}" if proc.returncode else ""))
    if proc.returncode == 0:
        with open(out_csv, encoding="utf-8", newline="") as f:
            rows = list(csv.reader(f))
        check(rows[0][3:] == LABELS, f"CSV headers keep Unicode labels {rows[0][3:]}")
        compare({Path(r[0]).name: r[3:] for r in rows[1:]}, truth, "CLI")

    try:
        if (ROOT / "frontend" / "server.py").is_file():
            check_server(work, truth, ref)
        if GUI.is_file():
            check_gui(work)
    finally:
        shutil.rmtree(work.parent, ignore_errors=True)

    print("\nPASS" if not failures else f"\nFAILED ({len(failures)} check(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
