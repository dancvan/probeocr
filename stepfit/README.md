# stepfit

Turn a recorded **step response** into a **stable zpk model** of the system's transfer
function, over a frequency range you choose.

1. Import a CSV (or `.dat`/`.tsv`/whitespace table) and pick the **time**, **input** and
   **output** columns. Unrelated columns are ignored.
2. The tool finds the step, removes baselines, and estimates `H(f)` with its noise floor.
3. Choose a fit range (type it, or drag across the magnitude plot). It searches model
   orders, fits poles and zeros, and shows the model against the data, in the time domain
   too, with the result as a `scipy.signal.ZerosPolesGain` snippet.

## Run

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python server.py            # web UI at http://127.0.0.1:8766  (use "Try demo data")
.venv/bin/python cli.py samples/thermal_step.csv --fmin 3e-4 --fmax 2e-2   # no browser
.venv/bin/python -m pytest tests
```

CLI columns can be named or indexed. Leave `--input` out for a unit step at the first
sample (add `--amplitude` to scale the gain), and use `--dt` instead of `--time` for evenly
spaced rows. `--json` saves the model and diagnostics.

## How it works

**Transfer function.** For a step of size `A`, the impulse response is `dy/A`, so
`H(f) = Σ Δy·e^(−j2πf·t) / A`, evaluated on a log grid of frequencies instead of an FFT grid
(the notebook's `freqz` of the differenced data, generalised). Differencing is corrected for
its sinc roll-off, and the DC gain comes from the settled tail. *Measured input* mode divides
the spectrum of `Δy` by that of `Δu`, so a slow or shaped input edge is handled. Noise is
estimated from the second difference of the output, which gives a per-frequency noise floor
(`|H|` below 3σ is not trustworthy) and a suggested fit range.

**Fitting.** Weighted by SNR, so noisy points count less. A vector-fitting pass and several
log-spaced starts each seed a bounded least-squares fit of the log-magnitude and phase error.
The model order is searched: the simplest model within 20% of the best error wins, and the
search is capped at about two poles per decade of fit range so it cannot chase noise.

**Stability outside the fit range, by construction.** Every pole and zero is a negative
real number or a conjugate pair with damping in `[min_damping, 0.999]`, and its frequency
is bounded to `[f_lo/guard, f_hi·guard]`. The optimiser cannot leave that box, so the model:

- is stable and minimum phase (zeros included, so it can be inverted);
- has no pole or zero at DC or far below the range, so it flattens to a finite gain, by
  default pinned to the measured DC gain, instead of extrapolating a slope;
- has no resonance far above the range, and rolls off at `−20·(poles − zeros)` dB/decade
  (`roll-off` ≥ 1 keeps it strictly proper).

The result panel checks all of this on the final model and plots it three decades beyond
the data so you can see it.

## Assumptions and limits

- Linear, time-invariant, and starting from rest; the output has to **settle** before the
  record ends. The tool warns if it has not.
- Finite, non-zero DC gain. Integrators, differentiators and pure delays are not modelled
  (zeros are minimum phase only).
- Roughly uniform sampling. The noise estimate assumes white noise; drifting, colored noise
  makes it optimistic, which is why the order cap exists.
- Frequencies above ~0.9× Nyquist are not estimated.
- Everything runs locally; the server binds to 127.0.0.1.
