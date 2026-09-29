#!/usr/bin/env python3
"""Generate synthetic probe screenshots (requires ImageMagick's `magick`).

Each image shows an instrument panel with three probe readouts. The panel is
jittered by a few pixels per image to mimic windows that don't sit in exactly
the same place. Ground truth is written to truth.csv.

    python3 tools/make_samples.py samples/ 12
"""
import csv
import random
import subprocess
import sys
from pathlib import Path

FONT = "/System/Library/Fonts/Helvetica.ttc"
PROBES = [("CH1", "°C", 60), ("CH2", "kPa", 150), ("CH3", "mV", 240)]


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "samples")
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(7)

    with open(out / "truth.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["image"] + [p[0] for p in PROBES])
        for i in range(count):
            ox, oy = rng.randint(-18, 18), rng.randint(-12, 12)
            values = [f"{rng.uniform(-40, 400):.2f}" for _ in PROBES]
            draw = [
                "-fill", "#1e2530", "-draw", f"rectangle {80+ox},{40+oy} {560+ox},{330+oy}",
                "-fill", "#9fb4c8", "-pointsize", "20",
                "-draw", f"text {100+ox},{75+oy} 'Probe Monitor  v2.1'",
            ]
            for (name, unit, y), val in zip(PROBES, values):
                draw += [
                    "-fill", "#9fb4c8", "-pointsize", "22",
                    "-draw", f"text {110+ox},{y+60+oy} '{name}'",
                    "-fill", "#6ff08a", "-pointsize", "40",
                    "-draw", f"text {220+ox},{y+66+oy} '{val}'",
                    "-fill", "#9fb4c8", "-pointsize", "22",
                    "-draw", f"text {450+ox},{y+60+oy} '{unit}'",
                ]
            name = f"shot_{i:03d}.png"
            subprocess.run(["magick", "-size", "640x400", "xc:#3a3f47", "-font", FONT, *draw,
                            str(out / name)], check=True)
            w.writerow([name] + values)
    print(f"wrote {count} images to {out}/")


if __name__ == "__main__":
    main()
