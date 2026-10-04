import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import core  # noqa: E402

TWO_PI = 2 * np.pi


def make_step(z_hz, p_hz, dc=1.0, dt=5.0, n=700, noise=0.0, amp=1.0, seed=0, pre=40):
    z = -TWO_PI * np.array(z_hz, float)
    p = -TWO_PI * np.array(p_hz, float)
    k = dc * np.prod(-p).real / np.prod(-z).real if len(z) else dc * np.prod(-p).real
    true = core.ZPK(z.astype(complex), p.astype(complex), float(k))
    t = np.arange(-pre, n) * dt
    u = np.where(t >= 0, amp, 0.0)
    y = np.zeros_like(t)
    ts = -dt / 2                      # the edge falls between two samples; midpoint is the estimate
    y[t >= 0] = true.step(t[t >= 0] - ts, amp)
    y += np.random.default_rng(seed).normal(0, noise, len(t))
    return true, t, u, y


def run(true, t, u, y, f_lo, f_hi, **kw):
    sd = core.prepare(t, y, u)
    resp = core.estimate_response(sd)
    return sd, resp, core.fit_zpk(resp, core.FitSettings(f_lo, f_hi, **kw))


def test_response_matches_truth():
    true, t, u, y = make_step([2e-3], [0.5e-3, 2.5e-3, 12e-3], dc=0.8, amp=0.95)
    sd = core.prepare(t, y, u)
    assert sd.settled and sd.amplitude == pytest.approx(0.95)
    resp = core.estimate_response(sd)
    sel = (resp.f > 1.5 / (700 * 5)) & (resp.f < 0.02)
    err = np.abs(resp.H[sel] / true.freqresp(resp.f[sel]) - 1)
    assert err.max() < 0.03
    assert resp.h0 == pytest.approx(0.8, rel=1e-3)


def test_fit_recovers_thermal_plant():
    true, t, u, y = make_step([2e-3], [0.5e-3, 2.5e-3, 12e-3], dc=0.8, noise=3e-4)
    _, resp, fit = run(true, t, u, y, 3e-4, 4e-2)
    assert fit.checks["stable"] and fit.checks["minimum_phase"]
    assert fit.checks["roots_inside_guard_band"]
    assert fit.rms_db < 0.3
    assert fit.model.dc_gain == pytest.approx(0.8, rel=1e-3)   # pinned to the measured DC gain
    assert fit.n_poles <= 4


def test_fit_resonant_system():
    # lightly damped second-order section: needs a complex pair
    w0, zeta = TWO_PI * 1e-2, 0.15
    p = np.array([complex(-zeta * w0, w0 * np.sqrt(1 - zeta ** 2))])
    p = np.r_[p, p.conj()]
    true = core.ZPK(np.array([]), p, float(w0 ** 2))
    t = np.arange(-20, 1500) * 0.5
    y = np.zeros_like(t)
    y[t >= 0] = true.step(t[t >= 0])
    u = (t >= 0).astype(float)
    _, resp, fit = run(true, t, u, y, 1e-3, 5e-2, n_poles=2, n_zeros=0)
    assert fit.rms_db < 0.2
    assert np.any(np.abs(fit.model.p.imag) > 0)
    assert fit.checks["stable"]


def test_bounds_hold_outside_fit_range():
    true, t, u, y = make_step([2e-3], [0.5e-3, 2.5e-3, 12e-3], noise=1e-3)
    _, resp, fit = run(true, t, u, y, 1e-3, 2e-2, guard=4.0)
    c = fit.checks
    assert c["roots_inside_guard_band"]
    assert c["lowest_root_hz"] >= 1e-3 / 4 * 0.999
    assert c["highest_root_hz"] <= 2e-2 * 4 * 1.001
    assert c["relative_degree"] >= 1
    assert c["peak_outside_fit_vs_inside"] < 1.05


def test_negative_step_and_no_input_column():
    true, t, u, y = make_step([], [1e-3], dc=-2.0, amp=-0.5)
    sd, resp, fit = run(true, t, u, y, 3e-4, 3e-2, n_poles=1, n_zeros=0)
    assert sd.amplitude == pytest.approx(-0.5)
    assert fit.model.dc_gain == pytest.approx(-2.0, rel=1e-3)
    # without an input column: unit step at the first sample, gain scaled by the amplitude
    sd2 = core.prepare(t[40:], y[40:], None)
    assert sd2.amplitude == 1.0


def test_measured_input_mode_handles_slow_edge():
    # input rises over 100 samples; the true plant is 1/(1+s/wp) driven by that input
    wp = TWO_PI * 2e-3
    t = np.arange(-40, 700) * 5.0
    u = np.clip(t / 500.0, 0, 1)
    y = np.zeros_like(t)
    for i in range(1, len(t)):                    # exact discretisation for piecewise-linear u
        a = np.exp(-wp * 5.0)
        du = u[i] - u[i - 1]
        y[i] = a * y[i - 1] + (1 - a) * u[i - 1] + du * (1 - (1 - a) / (wp * 5.0))
    with pytest.raises(core.StepFitError):        # the ideal-step method refuses it
        core.prepare(t, y, u)
    sd = core.prepare(t, y, u, strict_step=False)
    resp = core.estimate_response(sd, mode="measured")
    sel = (resp.f > 3e-4) & (resp.f < 1e-2)
    true = core.ZPK(np.array([]), np.array([-wp + 0j]), float(wp))
    err = np.abs(resp.H[sel] / true.freqresp(resp.f[sel]) - 1)
    assert err.max() < 0.05


def test_load_table_variants():
    a = core.load_table("t,u,y\n0,0,0.1\n1,1,0.2\n2,1,0.3\n")
    assert a.names == ["t", "u", "y"] and a.data.shape == (3, 3)
    b = core.load_table("# t u y\n0 0 0.1\n1 1 0.2\n")
    assert b.names == ["t", "u", "y"]
    c = core.load_table("1;2\n3;4\n")
    assert c.names == ["col0", "col1"]
    d = core.load_table("time,label,y\n0,a,1\n1,b,2\n2,c,3\n")
    assert d.usable == [True, False, True]
    with pytest.raises(core.StepFitError):
        core.load_table("# nothing\n")


def test_guess_columns_on_demo():
    tb = core.load_table(core.demo_csv())
    g = core.guess_columns(tb)
    assert tb.names[g["time"]] == "time_s"
    assert tb.names[g["input"]] == "power_W"
    assert tb.names[g["output"]] == "defocus_per_m"


def test_errors():
    t = np.arange(-10.0, 90.0)
    with pytest.raises(core.StepFitError):
        core.prepare(t, np.zeros(100), np.zeros(100))          # no step in input
    sd = core.prepare(t, 1 - np.exp(-np.maximum(t, 0) / 10), (t >= 0).astype(float))
    resp = core.estimate_response(sd)
    with pytest.raises(core.StepFitError):
        core.fit_zpk(resp, core.FitSettings(1e-9, 2e-9))        # no points in range
