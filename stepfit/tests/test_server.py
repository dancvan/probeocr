import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import core  # noqa: E402
import server  # noqa: E402


def test_parse_analyze_fit_roundtrip():
    j = server.parse({"name": "demo.csv", "text": core.demo_csv()})
    g = j["guess"]
    base = {"id": j["id"], "time": g["time"], "input": g["input"], "output": g["output"]}
    a = server.analyze(base)
    assert a["info"]["settled"] and abs(a["info"]["amplitude"] - 0.95) < 1e-9
    assert len(a["response"]["f"]) == len(a["response"]["mag"])
    f = server.fit({"id": j["id"], "f_lo": a["suggest"]["f_lo"], "f_hi": a["suggest"]["f_hi"]})
    assert f["checks"]["stable"] and f["checks"]["roots_inside_guard_band"]
    assert f["error"]["rms_db"] < 1.0
    import json
    json.dumps(f, allow_nan=False)             # everything must be valid JSON


def test_user_errors_are_stepfit_errors():
    j = server.parse({"text": core.demo_csv()})
    with pytest.raises(core.StepFitError):      # input == output
        server.analyze({"id": j["id"], "time": 0, "input": 2, "output": 2})
    with pytest.raises(core.StepFitError):      # non-step input column
        server.analyze({"id": j["id"], "time": 0, "input": 2, "output": 1})
    with pytest.raises(core.StepFitError):
        server.fit({"id": "nope", "f_lo": 1, "f_hi": 2})
