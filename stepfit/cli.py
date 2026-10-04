#!/usr/bin/env python3
"""Fit a stable zpk model to the step response in a CSV, without the web UI.

    python3 cli.py data.csv --time time_s --input power_W --output defocus_per_m \\
        --fmin 3e-4 --fmax 2e-2 [--poles 3 --zeros 1] [--json model.json]

Columns may be given by name or by 0-based index. Omit --input for a unit step at the
first sample; omit --time and give --dt for evenly spaced rows.
"""
import argparse
import json
import sys

import core
import report


def column(tb: core.Table, spec):
    if spec is None:
        return None
    if spec in tb.names:
        return tb.names.index(spec)
    if spec.lstrip("-").isdigit() and 0 <= int(spec) < len(tb.names):
        return int(spec)
    raise core.StepFitError(f"no column {spec!r}; have {', '.join(tb.names)}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("csv")
    ap.add_argument("--time"), ap.add_argument("--input"), ap.add_argument("--output")
    ap.add_argument("--dt", type=float, help="sample interval when there is no time column")
    ap.add_argument("--amplitude", type=float, help="override the step size")
    ap.add_argument("--mode", choices=["ideal", "measured"], default="ideal")
    ap.add_argument("--fmin", type=float, help="fit range lower edge, Hz (default: suggested)")
    ap.add_argument("--fmax", type=float, help="fit range upper edge, Hz (default: suggested)")
    ap.add_argument("--poles", type=int, help="fix the number of poles (default: search)")
    ap.add_argument("--zeros", type=int)
    ap.add_argument("--max-poles", type=int, default=6)
    ap.add_argument("--guard", type=float, default=10.0,
                    help="roots stay within [fmin/guard, fmax*guard]")
    ap.add_argument("--rolloff", type=int, default=1, help="minimum poles minus zeros")
    ap.add_argument("--min-damping", type=float, default=0.02,
                    help="lower bound on complex-pair damping ratio (lower it for sharp resonances)")
    ap.add_argument("--no-dc-match", action="store_true")
    ap.add_argument("--json", help="write the model and diagnostics to this file")
    a = ap.parse_args(argv)

    try:
        tb = core.load_table(open(a.csv, encoding="utf-8-sig", errors="replace").read())
        g = core.guess_columns(tb)
        ti, ui, yi = (column(tb, c) for c in (a.time, a.input, a.output))
        if a.time is None and a.dt is None:
            ti = g["time"]
        if a.input is None and a.dt is None and a.time is None:
            ui = g["input"]
        yi = g["output"] if yi is None else yi
        if yi is None:
            raise core.StepFitError("could not guess the output column; pass --output")
        col = lambda i: None if i is None else tb.data[:, i]              # noqa: E731
        sd = core.prepare(col(ti), col(yi), col(ui), dt=a.dt, amplitude=a.amplitude,
                          strict_step=a.mode == "ideal")
        resp = core.estimate_response(sd, mode=a.mode)
        cfg = core.FitSettings(
            a.fmin or resp.f_lo_suggest, a.fmax or resp.f_hi_suggest, a.poles, a.zeros,
            a.max_poles, a.guard, a.rolloff, not a.no_dc_match, a.min_damping)
        res = core.fit_zpk(resp, cfg)
    except (core.StepFitError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    names = tb.names
    print(f"time={names[ti] if ti is not None else f'dt={a.dt}'}  "
          f"input={names[ui] if ui is not None else '(unit step)'}  output={names[yi]}")
    print(f"step {sd.amplitude:g} at t={sd.t0:g}s, DC gain {sd.h0:.6g}, noise sigma {sd.sigma_y:.3g}")
    for n in sd.notes:
        print("note:", n)
    print(f"fit range {cfg.f_lo:.4g} - {cfg.f_hi:.4g} Hz  ->  {res.n_poles} poles, "
          f"{res.n_zeros} zeros   rms error {res.rms_db:.2f} dB / {res.rms_deg:.1f} deg")
    print()
    print(report.python_snippet(res.model))
    c = res.checks
    print(f"stable={c['stable']}  min-phase={c['minimum_phase']}  roll-off "
          f"{c['hf_slope_db_per_decade']} dB/dec  roots {c['lowest_root_hz']:.3g}-"
          f"{c['highest_root_hz']:.3g} Hz  DC gain {c['dc_gain_model']:.6g}")
    for w in res.warnings:
        print("warning:", w)
    if a.json:
        out = report.fit_json(sd, resp, cfg, res)
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump({k: out[k] for k in ("model", "order", "error", "checks", "warnings",
                                           "candidates")}, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
