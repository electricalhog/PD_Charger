"""Offline analysis of scope captures (CSV: t_s, CH1, CH2, ...).

Numbers come from the samples, not from the screen: per-channel levels,
frequency/duty, interpolated threshold crossings, and edge-to-edge delay
statistics over every period in the capture (e.g. dead-time).
"""

from __future__ import annotations

import csv
import math
import re
from pathlib import Path

from .config import ToolError


def load_csv(path: str | Path) -> tuple[list[float], dict[str, list[float]]]:
    p = Path(path)
    if not p.exists():
        raise ToolError(f"capture not found: {p}")
    with p.open(newline="") as fh:
        r = csv.reader(fh)
        header = next(r)
        cols: dict[str, list[float]] = {h: [] for h in header[1:]}
        t: list[float] = []
        for row in r:
            t.append(float(row[0]))
            for h, x in zip(header[1:], row[1:]):
                cols[h].append(float(x))
    return t, cols


def levels(v: list[float]) -> dict:
    """Robust low/high levels from a histogram of the samples (ignores ringing)."""
    if not v:
        return {}
    vmin, vmax = min(v), max(v)
    n = len(v)
    # Median of the samples on each side of the min/max midpoint: works at any duty
    # cycle (percentiles fail at 98 %) and ignores ringing/overshoot at the edges.
    mid = (vmin + vmax) / 2
    lo_s = sorted(x for x in v if x < mid)
    hi_s = sorted(x for x in v if x >= mid)
    low = lo_s[len(lo_s) // 2] if lo_s else vmin
    high = hi_s[len(hi_s) // 2] if hi_s else vmax
    mean = sum(v) / n
    return {"min": vmin, "max": vmax, "low": low, "high": high, "mean": mean,
            "rms": math.sqrt(sum(x * x for x in v) / n)}


def crossings(v: list[float], t: list[float], threshold: float, hysteresis: float) -> tuple[list[float], list[float]]:
    """Rising/falling threshold-crossing times, linearly interpolated, with hysteresis."""
    rising, falling, state = [], [], None
    last_below = last_above = None  # index of last sample on each side of threshold
    for i, x in enumerate(v):
        if x < threshold:
            last_below = i
        else:
            last_above = i
        if state is not True and x > threshold + hysteresis:
            if state is False and last_below is not None:
                j = last_below
                rising.append(_interp(t, v, j, threshold))
            state = True
        elif state is not False and x < threshold - hysteresis:
            if state is True and last_above is not None:
                j = last_above
                falling.append(_interp(t, v, j, threshold))
            state = False
    return rising, falling


def _interp(t: list[float], v: list[float], j: int, th: float) -> float:
    """Time where the segment j -> j+1 crosses th."""
    if j + 1 >= len(v):
        return t[j]
    a, b = v[j], v[j + 1]
    frac = (th - a) / (b - a) if b != a else 0.0
    return t[j] + frac * (t[j + 1] - t[j])


def channel_report(v: list[float], t: list[float], threshold: float | None = None, limit: int = 20) -> dict:
    lv = levels(v)
    if not lv:
        return {}
    swing = lv["high"] - lv["low"]
    th = (lv["low"] + lv["high"]) / 2 if threshold is None else threshold
    rep = {**{k: round(x, 5) for k, x in lv.items()}, "points": len(v),
           "sample_interval_s": (t[-1] - t[0]) / (len(t) - 1) if len(t) > 1 else None, "threshold_v": round(th, 4)}
    if swing < 0.05:
        rep["edges"] = {"rising": 0, "falling": 0}
        return rep
    rising, falling = crossings(v, t, th, swing * 0.1)
    rep["edges"] = {"rising": len(rising), "falling": len(falling),
                    "rising_s": rising[:limit], "falling_s": falling[:limit]}
    if len(rising) >= 2:
        periods = [b - a for a, b in zip(rising, rising[1:])]
        per = sum(periods) / len(periods)
        rep["period_s"] = per
        rep["freq_hz"] = 1 / per
        rep["period_jitter_s"] = max(periods) - min(periods)
        highs = [next((f for f in falling if f > r), None) for r in rising]
        widths = [f - r for r, f in zip(rising, highs) if f is not None and f - r < per]
        if widths:
            w = sum(widths) / len(widths)
            rep["high_time_s"] = w
            rep["duty_pct"] = 100 * w / per
    return rep


_EDGE = {"rise": 0, "rising": 0, "r": 0, "fall": 1, "falling": 1, "f": 1}


def parse_delay(spec: str) -> tuple[str, int, str, int]:
    """'CH2:fall,CH3:rise' -> ('CH2', 1, 'CH3', 0)."""
    m = re.fullmatch(r"\s*(\w+):(\w+)\s*,\s*(\w+):(\w+)\s*", spec)
    if not m or m[2].lower() not in _EDGE or m[4].lower() not in _EDGE:
        raise ToolError(f"bad delay spec {spec!r}; use e.g. CH2:fall,CH3:rise")
    return m[1].upper(), _EDGE[m[2].lower()], m[3].upper(), _EDGE[m[4].lower()]


def delay_stats(cols: dict[str, list[float]], t: list[float], spec: str,
                thresholds: dict[str, float | None]) -> dict:
    """For every <A edge>, the time to the next <B edge> (e.g. dead-time per switching cycle)."""
    a, ea, b, eb = parse_delay(spec)
    for ch in (a, b):
        if ch not in cols:
            raise ToolError(f"{ch} not in capture", channels=list(cols))
    edges = {}
    for ch in {a, b}:
        v = cols[ch]
        lv = levels(v)
        th = thresholds.get(ch)
        th = (lv["low"] + lv["high"]) / 2 if th is None else th
        edges[ch] = crossings(v, t, th, (lv["high"] - lv["low"]) * 0.1)
    src, dst = edges[a][ea], edges[b][eb]
    # Only pair an A edge with a B edge that comes before the next A edge.
    ds = []
    for i, ta in enumerate(src):
        nxt = src[i + 1] if i + 1 < len(src) else float("inf")
        tb = next((x for x in dst if ta <= x < nxt), None)
        if tb is not None:
            ds.append(tb - ta)
    out = {"spec": spec, "count": len(ds)}
    if ds:
        mean = sum(ds) / len(ds)
        out.update(mean_s=mean, min_s=min(ds), max_s=max(ds),
                   std_s=math.sqrt(sum((d - mean) ** 2 for d in ds) / len(ds)), first_s=ds[:10])
    else:
        out["note"] = "no matching edge pairs (check edge types, thresholds and that both edges are in the capture)"
    return out


def _resolve(th: dict, cols: dict) -> dict:
    default = th.get("*")
    return {ch: th.get(ch, default) for ch in cols}


def analyze(path: str | Path, thresholds: dict[str, float | None] | None = None,
            delays: list[str] | None = None) -> dict:
    t, cols = load_csv(path)
    thresholds = _resolve(thresholds or {}, cols)
    out = {"ok": True, "file": str(path), "duration_s": t[-1] - t[0] if t else 0,
           "channels": {ch: channel_report(v, t, thresholds.get(ch)) for ch, v in cols.items()}}
    if delays:
        out["delays"] = [delay_stats(cols, t, d, thresholds) for d in delays]
    return out
