"""JSON-friendly summaries of an analysis / fit, shared by the web server and the CLI."""
from __future__ import annotations

import math

import numpy as np

import core

MAX_PLOT_POINTS = 3000


def jl(x):
    """numpy -> plain list, non-finite numbers become None (valid JSON)."""
    out = np.asarray(x, float).tolist()
    return [v if v is None or math.isfinite(v) else None for v in out]


def _stride(n, limit=MAX_PLOT_POINTS):
    return slice(None, None, max(1, math.ceil(n / limit)))


def analysis_json(sd: core.StepData, resp: core.FreqResponse) -> dict:
    s = _stride(len(sd.t))
    return {
        "series": {"t": jl(sd.t[s]), "y": jl(sd.y[s]),
                   "u": jl(sd.u[s]) if sd.u is not None else None},
        "info": {"amplitude": sd.amplitude, "t0": sd.t0, "y0": sd.y0, "dc_gain": sd.h0,
                 "sigma_y": sd.sigma_y, "dt": sd.dt, "settled": sd.settled,
                 "n_samples": len(sd.t), "notes": sd.notes, "mode": resp.mode,
                 "fnyq": resp.fnyq},
        "response": {"f": jl(resp.f), "mag": jl(np.abs(resp.H)),
                     "phase": jl(np.degrees(np.unwrap(np.angle(resp.H)))),
                     "noise": jl(resp.sigma)},
        "suggest": {"f_lo": resp.f_lo_suggest, "f_hi": resp.f_hi_suggest},
    }


def _fmt_c(r: complex) -> str:
    return f"{r.real:.6g}" if abs(r.imag) <= 1e-9 * abs(r) else f"{r.real:.6g}{r.imag:+.6g}j"


def python_snippet(model: core.ZPK) -> str:
    z = ", ".join(_fmt_c(r) for r in model.z)
    p = ", ".join(_fmt_c(r) for r in model.p)
    return ("from scipy import signal\n\n"
            "# roots in rad/s; G(s) = k * prod(s - z) / prod(s - p)\n"
            f"z = [{z}]\n"
            f"p = [{p}]\n"
            f"k = {model.k:.9g}\n"
            "G = signal.ZerosPolesGain(z, p, k)\n")


def fit_json(sd, resp, cfg, fit: core.FitResult) -> dict:
    lo, hi = cfg.f_lo, cfg.f_hi
    wide = np.logspace(math.log10(lo) - 3, math.log10(hi) + 3, 700)
    G = fit.model.freqresp(wide)
    post = sd.t >= 0
    tt, yy = sd.t[post], sd.y[post]
    s = _stride(len(tt), 1500)
    tt, yy = tt[s], yy[s]
    ym = fit.model.step(tt, sd.amplitude)
    return {
        "model": fit.model.to_dict(),
        "order": {"poles": fit.n_poles, "zeros": fit.n_zeros},
        "error": {"rms_db": fit.rms_db, "rms_deg": fit.rms_deg,
                  "max_db": fit.max_db, "max_deg": fit.max_deg, "cost": fit.cost},
        "curve": {"f": jl(wide), "mag": jl(np.abs(G)),
                  "phase": jl(np.degrees(np.unwrap(np.angle(G))))},
        "residual": {"f": jl(fit.f_fit), "db": jl(fit.err_db), "deg": jl(fit.err_deg)},
        "step": {"t": jl(tt), "measured": jl(yy), "model": jl(ym),
                 "rms": float(np.sqrt(np.mean((ym - yy) ** 2)))},
        "checks": fit.checks,
        "warnings": fit.warnings,
        "candidates": fit.candidates,
        "python": python_snippet(fit.model),
    }
