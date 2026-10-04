"""stepfit core: step-response CSV -> frequency response -> stable zpk model.

Pipeline
    load_table      parse a CSV / .dat / whitespace table
    prepare         pick the step out of the input column, remove baselines, measure noise
    estimate_response   H(f) = DTFT[dy] / DTFT[du]   (or / A for an ideal step)
    fit_zpk         vector fitting for a start, then bounded least squares on the roots

Stability by construction: every pole and zero is parametrised as a negative real number
or a conjugate pair with positive damping, and its frequency is bounded to
[f_lo / guard, f_hi * guard]. The optimiser cannot leave that box, so nothing can drift
unstable, sit at DC, or ring far outside the fitting range.
"""
from __future__ import annotations

import csv
import math
from dataclasses import dataclass, field

import numpy as np
from scipy import optimize, signal

TWO_PI = 2.0 * np.pi


class StepFitError(ValueError):
    """A problem the user can fix (bad column, no step, range too narrow, ...)."""


# --------------------------------------------------------------------------- import

@dataclass
class Table:
    names: list[str]
    data: np.ndarray          # rows x cols, NaN where a cell was not a number
    usable: list[bool]        # column is (almost) entirely numeric


_COMMENT = ("#", "%", "//")


def _delimiter(line: str):
    for d in (",", ";", "\t"):
        if d in line:
            return d
    return None               # whitespace


def _split(line: str, delim):
    if delim is None:
        return line.split()
    return [c.strip() for c in next(csv.reader([line], delimiter=delim))]


def _num(s: str) -> float:
    try:
        v = float(s)
    except ValueError:
        return math.nan
    return v if math.isfinite(v) else math.nan


def load_table(text: str) -> Table:
    delim, header, comment_header = None, None, None
    ncols, rows = None, []
    for raw in text.lstrip("﻿").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith(_COMMENT):
            if not rows and header is None:
                comment_header = line.lstrip("#%/ ").strip()   # "# t, u, y" style header
            continue
        if ncols is None:
            delim = _delimiter(line)
            fields = _split(line, delim)
            if all(math.isnan(_num(f)) for f in fields):       # first real line is a header
                header = fields
                continue
            ncols = len(fields)
            if header is None and comment_header:
                header = _split(comment_header, delim)
            if header is not None and len(header) != ncols:
                header = None
        else:
            fields = _split(line, delim)
        if ncols is None:
            continue
        if len(fields) != ncols:
            continue
        vals = [_num(f) for f in fields]
        if not all(math.isnan(v) for v in vals):
            rows.append(vals)
    if not rows:
        raise StepFitError("no numeric rows found in the file")
    data = np.array(rows, dtype=float)
    names = [h.strip('"\' ') or f"col{i}" for i, h in enumerate(header)] if header \
        else [f"col{i}" for i in range(ncols)]
    usable = [bool(np.mean(np.isfinite(data[:, j])) >= 0.9) for j in range(ncols)]
    return Table(names, data, usable)


def guess_columns(tb: Table) -> dict:
    """Best guess for (time, input, output) column indices; any may be None."""
    cols = [j for j, ok in enumerate(tb.usable) if ok]
    tcol = None
    for j in cols:
        x = tb.data[:, j]
        x = x[np.isfinite(x)]
        if len(x) > 3 and np.all(np.diff(x) > 0):
            tcol = j
            break
    rest = [j for j in cols if j != tcol]
    icol = None
    for j in rest:
        x = tb.data[:, j]
        x = x[np.isfinite(x)]
        span = x.max() - x.min()
        if span > 0 and np.mean((x < x.min() + 0.05 * span) | (x > x.max() - 0.05 * span)) > 0.9:
            icol = j                       # sits on two levels: looks like a step
            break
    outs = [j for j in rest if j != icol]
    return {"time": tcol, "input": icol, "output": outs[0] if outs else None}


# ------------------------------------------------------------------- step analysis

@dataclass
class StepData:
    t: np.ndarray                 # s, relative to the step
    y: np.ndarray                 # output, baseline removed
    u: np.ndarray | None          # input, baseline removed (None if no input column)
    amplitude: float              # signed step size
    t0: float                     # absolute time of the step in the file
    y0: float
    h0: float                     # DC gain from the settled tail
    settled: bool
    sigma_y: float                # white-noise estimate on y
    dt: float
    notes: list[str] = field(default_factory=list)


def _robust_sigma(x: np.ndarray) -> float:
    return 1.4826 * float(np.median(np.abs(x - np.median(x))))


def prepare(t, y, u=None, *, dt=None, amplitude=None, step_time=None,
            strict_step=True) -> StepData:
    """Locate the step, remove baselines, estimate noise and DC gain.

    strict_step: require the input to sit on two levels (ideal-step method). Turn it off for
    the measured-input method, where a slow edge is fine.
    """
    y = np.asarray(y, float)
    if t is None:
        if not dt or dt <= 0:
            raise StepFitError("need a time column or a positive sample interval")
        t = np.arange(len(y)) * float(dt)
    t = np.asarray(t, float)
    ok = np.isfinite(t) & np.isfinite(y)
    if u is not None:
        u = np.asarray(u, float)
        ok &= np.isfinite(u)
    t, y = t[ok], y[ok]
    u = u[ok] if u is not None else None
    notes = []
    order = np.argsort(t, kind="stable")
    if not np.all(order == np.arange(len(t))):
        t, y = t[order], y[order]
        u = u[order] if u is not None else None
        notes.append("time column was not sorted; rows were re-ordered")
    keep = np.r_[True, np.diff(t) > 0]
    if not keep.all():
        t, y = t[keep], y[keep]
        u = u[keep] if u is not None else None
        notes.append(f"dropped {int((~keep).sum())} rows with repeated timestamps")
    if len(t) < 20:
        raise StepFitError("need at least 20 samples")

    n = len(t)
    k = max(3, n // 20)
    if u is not None:
        lo_, hi_ = float(u.min()), float(u.max())
        mid = 0.5 * (lo_ + hi_)
        up = u >= mid
        sig_u = _robust_sigma(np.diff(u)) / math.sqrt(2)
        if hi_ - lo_ <= 5 * sig_u or up[0] == up[-1] or up.all() or not up.any():
            raise StepFitError("the input column does not contain a single step "
                               "(it must start on one level and end on another)")
        level = lambda m: float(np.median(u[m]))          # noqa: E731
        u_lo, u_hi = level(up == up[0]), level(up == up[-1])
        A = u_hi - u_lo
        near = (np.abs(u - u_lo) < 0.1 * abs(A)) | (np.abs(u - u_hi) < 0.1 * abs(A))
        if strict_step and near.mean() < 0.9:
            raise StepFitError("the input column is not a clean step (under 90% of its samples "
                               "sit on the two levels); for a slow or shaped input use the "
                               "'measured input' method")
        frac = (u - u_lo) / A
        i = int(np.argmax(frac >= 0.5))
        if i == 0:
            t0 = t[0]
        else:
            t0 = t[i - 1] + (0.5 - frac[i - 1]) / (frac[i] - frac[i - 1]) * (t[i] - t[i - 1])
        ub = u - u_lo
    else:
        A, t0, ub = 1.0, t[0], None
        notes.append("no input column: assuming a unit step at the first sample "
                     "(set amplitude to scale the gain)")
    if step_time is not None:
        t0 = float(step_time)
    if amplitude is not None:
        A = float(amplitude)
    if A == 0:
        raise StepFitError("step amplitude is zero")

    pre = y[t < t0 - 1e-12 * max(1.0, abs(t0))]
    y0 = float(np.median(pre)) if len(pre) >= 3 else float(y[0])
    yb = y - y0
    tr = t - t0
    post = tr >= 0
    if post.sum() < 20:
        raise StepFitError("fewer than 20 samples after the step")

    d2 = np.diff(yb[post], 2)
    sigma_y = _robust_sigma(d2) / math.sqrt(6.0)       # var(second difference) = 6 sigma^2
    dts = np.diff(tr[post])
    dt_med = float(np.median(dts))
    if np.max(dts) > 5 * dt_med:
        notes.append("sampling is far from uniform; frequency estimates near Nyquist "
                     "are approximate")

    tail = yb[post][-max(10, int(post.sum()) // 10):]
    half = len(tail) // 2
    m1, m2 = float(np.mean(tail[:half])), float(np.mean(tail[half:]))
    y_dc = float(np.mean(tail))
    drift = abs(m2 - m1)
    settled = drift <= 0.01 * abs(y_dc) or drift <= 4 * sigma_y * math.sqrt(2.0 / half)
    if not settled:
        notes.append("the output has not settled by the end of the record; the DC gain and "
                     "the lowest frequencies are unreliable")
    return StepData(tr, yb, ub, A, float(t0), y0, y_dc / A, bool(settled),
                    float(sigma_y), dt_med, notes)


# ----------------------------------------------------------- frequency response

@dataclass
class FreqResponse:
    f: np.ndarray                 # Hz
    H: np.ndarray                 # complex
    sigma: np.ndarray             # standard deviation of the estimate due to noise
    h0: float
    fnyq: float
    f_lo_suggest: float
    f_hi_suggest: float
    mode: str


def _dtft(x, tm, f):
    out = np.empty(len(f), complex)
    block = max(1, int(2e6 // max(1, len(tm))))
    for i in range(0, len(f), block):
        fb = f[i:i + block]
        out[i:i + block] = np.exp(-2j * np.pi * np.outer(fb, tm)) @ x
    return out


def estimate_response(sd: StepData, mode="ideal", fmin=None, fmax=None,
                      per_decade=40) -> FreqResponse:
    """H(f) from the *differences* of the samples (the impulse response).

    ideal     the input is an ideal step of size `amplitude` at t0: H = DTFT[dy] / A
    measured  use the recorded input too: H = DTFT[dy] / DTFT[du]; no step assumption
    """
    if mode not in ("ideal", "measured"):
        raise StepFitError(f"unknown mode {mode!r}")
    if mode == "measured":
        if sd.u is None:
            raise StepFitError("measured-input mode needs an input column")
        t, y, u = sd.t, sd.y, sd.u
    else:
        m = sd.t >= 0
        t, y, u = sd.t[m], sd.y[m], None
        if t[0] > 1e-12 * max(1.0, abs(t[-1])):
            t, y = np.r_[0.0, t], np.r_[0.0, y]      # output is at baseline until the step
    span = t[-1] - t[0]
    fnyq = 0.5 / sd.dt
    fmin = 0.5 / span if fmin is None else fmin
    fmax = 0.9 * fnyq if fmax is None else min(fmax, 0.95 * fnyq)
    if not fmax > fmin:
        raise StepFitError("record is too short for the requested frequency span")
    nf = max(8, int(math.ceil(math.log10(fmax / fmin) * per_decade)) + 1)
    f = np.logspace(math.log10(fmin), math.log10(fmax), nf)

    tm = 0.5 * (t[1:] + t[:-1])
    num = _dtft(np.diff(y), tm, f)
    if u is None:
        den = sd.amplitude * np.sinc(f * sd.dt)       # undo the sinc of differencing
    else:
        den = _dtft(np.diff(u), tm, f)
    H = num / den
    sigma = sd.sigma_y * 2 * np.abs(np.sin(np.pi * f * sd.dt)) * math.sqrt(len(tm)) / np.abs(den)

    noisy = np.abs(H) < 3 * sigma
    run = noisy[:-2] & noisy[1:-1] & noisy[2:]
    f_hi = float(f[int(np.argmax(run))]) if run.any() else float(f[-1])
    return FreqResponse(f, H, sigma, sd.h0, fnyq, max(fmin, 1.0 / span), f_hi, mode)


# --------------------------------------------------------------------- the model

@dataclass
class ZPK:
    z: np.ndarray                 # rad/s (complex, conjugate-closed)
    p: np.ndarray
    k: float                      # G(s) = k prod(s - z) / prod(s - p)

    def freqresp(self, f_hz):
        s = 2j * np.pi * np.asarray(f_hz, float)[:, None]
        lz = np.log(s - self.z[None, :]).sum(1)
        lp = np.log(s - self.p[None, :]).sum(1)
        return self.k * np.exp(lz - lp)

    @property
    def dc_gain(self) -> float:
        return float(self.k * np.prod(-self.z).real / np.prod(-self.p).real)

    def step(self, t, amplitude=1.0):
        """Step response sampled at t (computed in time-scaled units for conditioning)."""
        t = np.asarray(t, float)
        wc = float(np.exp(np.mean(np.log(np.abs(self.p)))))
        sys = signal.lti(self.z / wc, self.p / wc, self.k * wc ** (len(self.z) - len(self.p)))
        tu = np.linspace(0.0, max(t.max(), 1e-12), 4000) * wc
        _, yu = signal.step(sys, T=tu)
        return amplitude * np.interp(t * wc, tu, yu)

    def to_dict(self):
        def roots(r):
            return [{"re": float(x.real), "im": float(x.imag)} for x in r]
        return {
            "gain": float(self.k), "dc_gain": self.dc_gain,
            "zeros_rad": roots(self.z), "poles_rad": roots(self.p),
            "zeros_hz": roots(self.z / TWO_PI), "poles_hz": roots(self.p / TWO_PI),
        }


# ------------------------------------------------------------------------ fitting

@dataclass
class FitSettings:
    f_lo: float
    f_hi: float
    n_poles: int | None = None        # None = search
    n_zeros: int | None = None
    max_poles: int = 6
    guard: float = 10.0               # roots must lie in [f_lo/guard, f_hi*guard]
    rolloff: int = 1                  # minimum (poles - zeros): high-frequency roll-off
    match_dc: bool = True             # pin the DC gain to the measured one
    tol: float = 0.03                 # accept the simplest model within this rms log error
    min_damping: float = 0.02         # lower bound on complex-pair damping ratio
    snr_full: float = 30.0            # weight = min(1, SNR / snr_full): noisy points count less


@dataclass
class FitResult:
    model: ZPK
    n_poles: int
    n_zeros: int
    cost: float                       # weighted rms of ln(G/H) over the fit range
    rms_db: float
    rms_deg: float
    max_db: float
    max_deg: float
    checks: dict
    warnings: list[str]
    candidates: list[dict]
    f_fit: np.ndarray
    err_db: np.ndarray
    err_deg: np.ndarray


class _Ctx:
    """Normalised problem data (frequency scaled so the band's geometric centre is 1)."""

    def __init__(self, s, H, w, lo, hi, guard_log, h0, pin, zmin):
        self.s, self.H, self.w, self.h0, self.pin, self.zmin = s, H, w, h0, pin, zmin
        self.lo, self.hi, self.guard_log = lo, hi, guard_log      # root bounds (log, normalised)
        self.band = hi - guard_log                                # in-band half width (log)
        self.sign = 1.0 if (h0 if pin else H[0].real) >= 0 else -1.0


_ZETA_MAX = 0.999


def _take(theta, i, nr, nc):
    out = list((-np.exp(theta[i:i + nr])).astype(complex))
    i += nr
    for _ in range(nc):
        w0, zeta = math.exp(theta[i]), theta[i + 1]
        r = w0 * math.sqrt(1 - zeta * zeta)
        out += [complex(-w0 * zeta, r), complex(-w0 * zeta, -r)]
        i += 2
    return np.array(out, complex), i


def _unpack(theta, st, ctx):
    pr, pc, zr, zc = st
    p, i = _take(theta, 0, pr, pc)
    z, i = _take(theta, i, zr, zc)
    if ctx.pin:
        k = ctx.h0 * np.prod(-p).real / np.prod(-z).real
    else:
        k = ctx.sign * math.exp(theta[i])
    return z, p, float(k)


def _eval(z, p, k, s):
    lz = np.log(s[:, None] - z[None, :]).sum(1) if len(z) else 0.0
    lp = np.log(s[:, None] - p[None, :]).sum(1)
    return k * np.exp(lz - lp)


def _roots_to_theta(roots, zmin):
    """Split roots into reals and conjugate pairs; return (theta pieces, nr, nc)."""
    roots = np.asarray(roots, complex)
    reals = sorted(r.real for r in roots if abs(r.imag) <= 1e-6 * max(abs(r), 1e-30))
    pairs = sorted((r for r in roots if r.imag > 1e-6 * abs(r)), key=lambda r: abs(r))
    th = [math.log(max(abs(r), 1e-30)) for r in reals]
    for r in pairs:
        th += [math.log(abs(r)), min(max(-r.real / abs(r), zmin), _ZETA_MAX)]
    return th, len(reals), len(pairs)


def _bounds(st, ctx):
    pr, pc, zr, zc = st
    lb, ub = [], []
    for nr, nc in ((pr, pc), (zr, zc)):
        lb += [ctx.lo] * nr
        ub += [ctx.hi] * nr
        for _ in range(nc):
            lb += [ctx.lo, ctx.zmin]
            ub += [ctx.hi, _ZETA_MAX]
    if not ctx.pin:
        lb.append(-60.0)
        ub.append(60.0)
    return np.array(lb), np.array(ub)


def _run_ls(st, x0, ctx):
    lb, ub = _bounds(st, ctx)
    x0 = np.clip(x0, lb + 1e-9, ub - 1e-9)

    def resid(th):
        z, p, k = _unpack(th, st, ctx)
        r = _eval(z, p, k, ctx.s) / ctx.H
        e = ctx.w * (np.log(np.abs(r) + 1e-300) + 1j * np.angle(r))
        return np.concatenate([e.real, e.imag])

    try:
        sol = optimize.least_squares(resid, x0, bounds=(lb, ub), method="trf",
                                     xtol=1e-10, ftol=1e-10, max_nfev=400)
    except (ValueError, FloatingPointError):
        return None
    if not np.all(np.isfinite(sol.x)):
        return None
    cost = math.sqrt(float(np.mean(sol.fun ** 2)) * 2)     # rms |e| over complex points
    return cost, sol.x


def _initial_thetas(n, m, ctx):
    """Log-spaced, interleaved real roots (two shifts), plus a vector-fitting start."""
    band = ctx.band
    p_pos = np.linspace(-band, band, n) if n > 1 else np.array([0.0])
    sp = (p_pos[1] - p_pos[0]) if n > 1 else 1.0
    starts = []
    for shift in (0.25, -0.25):
        z_pos = np.linspace(-band, band, m + 2)[1:-1] + shift * sp if m else np.array([])
        starts.append(((n, 0, m, 0), np.concatenate([p_pos, z_pos])))
    vf = _vf_start(n, m, ctx)
    if vf is not None:
        starts.append(vf)
    return starts


# ---- vector fitting (Gustavsen & Semlyen), real arithmetic on conjugate pairs

def _sort_poles(eigs):
    eigs = np.where(eigs.real > 0, -eigs.real + 1j * eigs.imag, eigs)
    reals = [e.real + 0j for e in eigs if abs(e.imag) <= 1e-6 * abs(e)]
    pairs = [e for e in eigs if e.imag > 1e-6 * abs(e)]
    out = sorted(reals, key=lambda e: e.real)
    for e in sorted(pairs, key=abs):
        out += [e, e.conjugate()]
    return np.array(out, complex)


def _basis(s, poles):
    cols, i = [], 0
    while i < len(poles):
        p = poles[i]
        if abs(p.imag) <= 1e-6 * abs(p):
            cols.append(1.0 / (s - p.real))
            i += 1
        else:
            a, b = 1 / (s - p), 1 / (s - p.conjugate())
            cols += [a + b, 1j * (a - b)]
            i += 2
    return np.stack(cols, 1)


def _lstsq(A, rhs, w):
    A, rhs = A * w[:, None], rhs * w
    Ar = np.vstack([A.real, A.imag])
    br = np.concatenate([rhs.real, rhs.imag])
    sc = np.linalg.norm(Ar, axis=0)
    sc[sc == 0] = 1
    return np.linalg.lstsq(Ar / sc, br, rcond=None)[0] / sc


def _vf_relocate(s, f, w, poles):
    Phi = _basis(s, poles)
    n = Phi.shape[1]
    x = _lstsq(np.hstack([Phi, np.ones((len(s), 1)), -f[:, None] * Phi]), f, w)
    ct = x[n + 1:]
    Am, b, i = np.zeros((n, n)), np.zeros(n), 0
    while i < n:
        p = poles[i]
        if abs(p.imag) <= 1e-6 * abs(p):
            Am[i, i], b[i] = p.real, 1.0
            i += 1
        else:
            Am[i:i + 2, i:i + 2] = [[p.real, p.imag], [-p.imag, p.real]]
            b[i] = 2.0
            i += 2
    return _sort_poles(np.linalg.eigvals(Am - np.outer(b, ct)))


def _vf_zpk(s, f, w, poles):
    """Partial-fraction fit with fixed poles -> (zeros, gain)."""
    n = len(poles)
    Phi = _basis(s, poles)
    x = _lstsq(np.hstack([Phi, np.ones((len(s), 1))]), f, w)
    c, d = x[:n], x[n]
    res, i = np.zeros(n, complex), 0
    while i < n:
        if abs(poles[i].imag) <= 1e-6 * abs(poles[i]):
            res[i] = c[i]
            i += 1
        else:
            res[i], res[i + 1] = c[i] + 1j * c[i + 1], c[i] - 1j * c[i + 1]
            i += 2
    den = np.poly(poles)
    num = d * den
    for j in range(n):
        num = num + res[j] * np.pad(np.poly(np.delete(poles, j)), (1, 0))
    num = np.real(num)
    num[np.abs(num) < 1e-9 * np.max(np.abs(num))] = 0.0
    nz = np.nonzero(num)[0]
    num = num[nz[0]:]
    return np.roots(num) if len(num) > 1 else np.array([]), float(num[0])


def _vf_start(n, m, ctx):
    try:
        band = ctx.band
        poles = _sort_poles(-np.exp(np.linspace(-band, band, n)).astype(complex)) if n > 1 \
            else np.array([-1.0 + 0j])
        for _ in range(12):
            poles = _vf_relocate(ctx.s, ctx.H, ctx.w, poles)
        zeros, _ = _vf_zpk(ctx.s, ctx.H, ctx.w, poles)
    except (np.linalg.LinAlgError, ValueError, FloatingPointError):
        return None
    zeros = np.where(zeros.real > 0, -zeros.real + 1j * zeros.imag, zeros)
    zeros = np.array(sorted(zeros, key=abs), complex)
    if m > len(zeros):
        return None
    keep = zeros[:m]                                  # drop the highest-frequency zeros
    # keep conjugate pairs intact
    if m and len(keep) and (np.sum(keep.imag > 1e-6 * np.abs(keep)) !=
                            np.sum(keep.imag < -1e-6 * np.abs(keep))):
        return None
    pth, pr, pc = _roots_to_theta(poles, ctx.zmin)
    zth, zr, zc = _roots_to_theta(keep, ctx.zmin)
    return (pr, pc, zr, zc), np.array(pth + zth)


def _fit_structure(n, m, ctx):
    best = None
    for st, th in _initial_thetas(n, m, ctx):
        pr, pc, zr, zc = st
        x0 = list(th)
        if not ctx.pin:
            z, p, _ = _unpack(np.array(x0 + [0.0]), st, ctx)
            g = abs(_eval(z, p, 1.0, ctx.s[:1])[0])
            x0.append(math.log(abs(ctx.H[0]) / g))
        out = _run_ls(st, np.array(x0), ctx)
        if out and (best is None or out[0] < best[0]):
            best = (out[0], st, out[1])
    if best is None:
        return None
    cost, st, th = best
    z, p, k = _unpack(th, st, ctx)
    return cost, z, p, k


def fit_zpk(resp: FreqResponse, cfg: FitSettings) -> FitResult:
    if not (0 < cfg.f_lo < cfg.f_hi):
        raise StepFitError("fit range must satisfy 0 < f_lo < f_hi")
    warns = []
    sel = (resp.f >= cfg.f_lo) & (resp.f <= cfg.f_hi)
    if sel.sum() < 10:
        raise StepFitError(f"only {int(sel.sum())} frequency points fall in the fit range "
                           f"({resp.f[0]:.3g}-{resp.f[-1]:.3g} Hz is available)")
    if cfg.f_lo < resp.f[0] or cfg.f_hi > resp.f[-1]:
        warns.append(f"fit range extends past the data ({resp.f[0]:.3g}-{resp.f[-1]:.3g} Hz)")
    f, H, sig = resp.f[sel], resp.H[sel], resp.sigma[sel]
    with np.errstate(divide="ignore", invalid="ignore"):
        snr = np.where(sig > 0, np.abs(H) / sig, np.inf)
    w = np.clip(snr / cfg.snr_full, 0.0, 1.0)
    if np.mean(snr < 3) > 0.3:
        warns.append(f"{100 * np.mean(snr < 3):.0f}% of the fit range is within 3x of the "
                     "noise floor; lower f_hi for a cleaner fit")

    wc = TWO_PI * math.sqrt(cfg.f_lo * cfg.f_hi)
    s = 2j * np.pi * f / wc
    pin = cfg.match_dc and abs(resp.h0) > 1e-3 * np.max(np.abs(H))
    if cfg.match_dc and not pin:
        warns.append("DC gain is ~0 relative to the band; not pinning it")
    lo = math.log(TWO_PI * cfg.f_lo / wc / cfg.guard)
    hi = math.log(TWO_PI * cfg.f_hi / wc * cfg.guard)
    ctx = _Ctx(s, H, w, lo, hi, math.log(cfg.guard), resp.h0, pin, cfg.min_damping)

    rolloff = max(0, cfg.rolloff)
    if cfg.n_poles is not None:
        if cfg.n_poles > 1 + math.ceil(2 * math.log10(cfg.f_hi / cfg.f_lo)):
            warns.append(f"{cfg.n_poles} poles over {math.log10(cfg.f_hi / cfg.f_lo):.1f} "
                         "decades is likely to over-fit the noise")
        grid = [(cfg.n_poles, cfg.n_zeros if cfg.n_zeros is not None
                 else max(0, cfg.n_poles - max(rolloff, 1)))]
        if grid[0][1] > grid[0][0] - rolloff:
            raise StepFitError(f"need at least {rolloff} more poles than zeros "
                               "(high-frequency roll-off)")
    else:
        decades = math.log10(cfg.f_hi / cfg.f_lo)
        n_max = min(cfg.max_poles, 1 + math.ceil(2 * decades))
        grid = [(n, m) for n in range(1, n_max + 1) for m in range(0, n - rolloff + 1)
                if cfg.n_zeros is None or m == cfg.n_zeros]
    cands = []
    n_max = max(n for n, _ in grid)
    for n, m in grid:
        out = _fit_structure(n, m, ctx)
        if out:
            cands.append({"n_poles": n, "n_zeros": m, "cost": out[0], "_fit": out[1:]})
    if not cands:
        raise StepFitError("the optimiser failed for every model order; "
                           "check the data and the fit range")
    best = min(c["cost"] for c in cands)
    ok = [c for c in cands if c["cost"] <= max(cfg.tol, 1.2 * best)]
    pick = min(ok, key=lambda c: (c["n_poles"] + c["n_zeros"], c["cost"]))
    z, p, k = pick["_fit"]
    n, m = pick["n_poles"], pick["n_zeros"]
    if cfg.n_poles is None and n == n_max < cfg.max_poles:
        warns.append(f"model order is capped at {n_max} poles because the fit range spans only "
                     f"{math.log10(cfg.f_hi / cfg.f_lo):.1f} decades (about 2 poles per decade "
                     "is what the data can support); widen the range or set the order by hand")
    model = ZPK(z * wc, p * wc, float(k * wc ** (n - m)))

    G = model.freqresp(f)
    ratio = G / H
    err_db = 20 * np.log10(np.abs(ratio))
    err_deg = np.degrees(np.angle(ratio))
    good = snr >= 10                  # report errors only where the data are trustworthy
    if not good.any():
        good = np.ones_like(good)
    chk = _checks(model, resp, cfg, n, m, warns)
    return FitResult(
        model, n, m, float(pick["cost"]),
        float(np.sqrt(np.mean(err_db[good] ** 2))), float(np.sqrt(np.mean(err_deg[good] ** 2))),
        float(np.max(np.abs(err_db[good]))), float(np.max(np.abs(err_deg[good]))),
        chk, warns,
        [{k_: v for k_, v in c.items() if k_ != "_fit"} for c in cands],
        f, err_db, err_deg)


def _checks(model, resp, cfg, n, m, warns) -> dict:
    p, z = model.p, model.z
    f_lo_root = np.min(np.abs(np.r_[p, z])) / TWO_PI if len(z) else np.min(np.abs(p)) / TWO_PI
    f_hi_root = np.max(np.abs(np.r_[p, z])) / TWO_PI
    wide = np.logspace(math.log10(cfg.f_lo) - 4, math.log10(cfg.f_hi) + 4, 800)
    mag = np.abs(model.freqresp(wide))
    inband = (wide >= cfg.f_lo) & (wide <= cfg.f_hi)
    ref = max(float(np.max(mag[inband])), abs(model.dc_gain))
    outside = float(np.max(mag[~inband]) / ref)
    dc_meas = resp.h0
    dc_err = (model.dc_gain - dc_meas) / dc_meas if dc_meas else float("nan")
    out = {
        "stable": bool(np.all(p.real < 0)),
        "minimum_phase": bool(np.all(z.real < 0)),
        "relative_degree": n - m,
        "hf_slope_db_per_decade": -20 * (n - m),
        "lowest_root_hz": float(f_lo_root),
        "highest_root_hz": float(f_hi_root),
        "roots_inside_guard_band": bool(f_lo_root >= cfg.f_lo / cfg.guard * 0.999 and
                                        f_hi_root <= cfg.f_hi * cfg.guard * 1.001),
        "peak_outside_fit_vs_inside": outside,
        "dc_gain_model": model.dc_gain,
        "dc_gain_measured": float(dc_meas),
        "dc_gain_error": float(dc_err),
    }
    if outside > 1.05:
        warns.append(f"model gain outside the fit range peaks {outside:.2f}x above the "
                     "in-band maximum")
    if abs(dc_err) > 0.05:
        warns.append(f"model DC gain differs from the measured value by {100 * dc_err:+.1f}%")
    return out


# ------------------------------------------------------------------------ demo data

def demo_csv(seed=1) -> str:
    """Noisy step response of a 3-pole / 1-zero thermal-like plant (time in s)."""
    rng = np.random.default_rng(seed)
    z = -2 * np.pi * np.array([2.0e-3])
    p = -2 * np.pi * np.array([0.5e-3, 2.5e-3, 12e-3])
    k = 0.8 * np.prod(-p) / np.prod(-z)
    t = np.arange(-300, 3000, 5.0)
    u = (t >= 0).astype(float) * 0.95
    y = np.zeros_like(t)
    m = t >= 0
    y[m] = ZPK(z.astype(complex), p.astype(complex), float(k)).step(t[m], 0.95)
    y += rng.normal(0, 4e-4, len(t)) + 0.02
    lines = ["time_s,power_W,defocus_per_m,unrelated_temp_C"]
    for a, b, c in zip(t, u, y):
        lines.append(f"{a:g},{b:g},{c:.6g},{21 + rng.normal(0, 0.05):.4g}")
    return "\n".join(lines) + "\n"
