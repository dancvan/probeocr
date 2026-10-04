#!/usr/bin/env python3
"""Local web frontend for stepfit.

    python3 server.py [--port 8766] [--no-browser]

Import a CSV that holds a step response, choose the time / input / output columns,
inspect the estimated transfer function, then fit a stable zpk model over a frequency
range you choose. Binds to 127.0.0.1 only; nothing leaves your machine.
"""
import argparse
import json
import sys
import threading
import uuid
import webbrowser
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np

import core
import report

STATIC = Path(__file__).resolve().parent / "static"
SESSIONS: "OrderedDict[str, dict]" = OrderedDict()
MAX_SESSIONS = 6


def new_session(table: core.Table, name: str) -> str:
    sid = uuid.uuid4().hex[:12]
    SESSIONS[sid] = {"table": table, "name": name}
    while len(SESSIONS) > MAX_SESSIONS:
        SESSIONS.popitem(last=False)
    return sid


def session(sid: str) -> dict:
    if sid not in SESSIONS:
        raise core.StepFitError("session expired; import the file again")
    return SESSIONS[sid]


def parse(data: dict) -> dict:
    tb = core.load_table(data["text"])
    sid = new_session(tb, data.get("name", ""))
    head = tb.data[:8]
    return {
        "id": sid, "name": data.get("name", ""), "rows": len(tb.data),
        "columns": [{"name": n, "usable": u} for n, u in zip(tb.names, tb.usable)],
        "preview": [[None if not np.isfinite(v) else float(v) for v in r] for r in head],
        "guess": core.guess_columns(tb),
    }


def _col(tb: core.Table, idx, what):
    if idx is None:
        return None
    if not isinstance(idx, int) or not 0 <= idx < tb.data.shape[1]:
        raise core.StepFitError(f"choose a valid {what} column")
    if not tb.usable[idx]:
        raise core.StepFitError(f"column '{tb.names[idx]}' is not numeric")
    return tb.data[:, idx]


def _opt_float(v):
    return None if v in (None, "") else float(v)


def analyze(data: dict) -> dict:
    ses = session(data["id"])
    tb = ses["table"]
    y = _col(tb, data.get("output"), "output")
    if y is None:
        raise core.StepFitError("choose an output column")
    t = _col(tb, data.get("time"), "time")
    u = _col(tb, data.get("input"), "input")
    if data.get("input") is not None and data.get("input") == data.get("output"):
        raise core.StepFitError("input and output must be different columns")
    mode = data.get("mode", "ideal")
    if mode == "measured" and u is None:
        raise core.StepFitError("measured-input mode needs an input column")
    sd = core.prepare(t, y, u, dt=_opt_float(data.get("dt")),
                      amplitude=_opt_float(data.get("amplitude")),
                      step_time=_opt_float(data.get("step_time")),
                      strict_step=mode == "ideal")
    resp = core.estimate_response(sd, mode=mode)
    ses["sd"], ses["resp"] = sd, resp
    return report.analysis_json(sd, resp)


def fit(data: dict) -> dict:
    ses = session(data["id"])
    if "resp" not in ses:
        raise core.StepFitError("analyse the data first")
    cfg = core.FitSettings(
        f_lo=float(data["f_lo"]), f_hi=float(data["f_hi"]),
        n_poles=int(data["n_poles"]) if data.get("n_poles") not in (None, "") else None,
        n_zeros=int(data["n_zeros"]) if data.get("n_zeros") not in (None, "") else None,
        max_poles=int(data.get("max_poles", 6)), guard=float(data.get("guard", 10)),
        rolloff=int(data.get("rolloff", 1)), match_dc=bool(data.get("match_dc", True)),
        min_damping=float(data.get("min_damping", 0.02)))
    if not 1 <= cfg.max_poles <= 10:
        raise core.StepFitError("max poles must be between 1 and 10")
    if cfg.guard < 1:
        raise core.StepFitError("guard factor must be >= 1")
    res = core.fit_zpk(ses["resp"], cfg)
    return report.fit_json(ses["sd"], ses["resp"], cfg, res)


class Handler(BaseHTTPRequestHandler):
    server_version = "stepfit"

    def log_message(self, *a):          # keep the terminal quiet
        pass

    def _send(self, body: bytes, ctype: str, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, obj, code=200):
        self._send(json.dumps(obj, allow_nan=False).encode(), "application/json", code)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            self._send((STATIC / "index.html").read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/demo":
            self.send_json({"name": "demo_step.csv", "text": core.demo_csv()})
        else:
            self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        route = {"/api/parse": parse, "/api/analyze": analyze, "/api/fit": fit}.get(
            self.path.split("?")[0])
        if route is None:
            return self.send_json({"error": "not found"}, 404)
        try:
            n = int(self.headers.get("Content-Length", 0))
            self.send_json(route(json.loads(self.rfile.read(n) or b"{}")))
        except (core.StepFitError, KeyError, ValueError, TypeError) as e:
            self.send_json({"error": str(e) or e.__class__.__name__}, 400)
        except Exception as e:           # a numerical failure should reach the user, not hang
            self.send_json({"error": f"{e.__class__.__name__}: {e}"}, 500)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{args.port}/"
    print(f"stepfit at {url}  (Ctrl-C to stop)")
    if not args.no_browser:
        threading.Timer(0.5, webbrowser.open, [url]).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
