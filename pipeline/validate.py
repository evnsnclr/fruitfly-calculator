"""How much does the calculator's 1 ms fixed-point step cost?

Runs every built-in scenario three ways and compares per-cell-type firing
rates:
  * the calculator model (integer, dt = 1 ms; pipeline.intsim, bit-exact with
    the C core),
  * the floating-point reference at dt = 0.1 ms (pipeline.refsim), twice with
    different random seeds, so the calculator-vs-reference difference can be
    judged against the reference's own run-to-run spread.

    python -m pipeline.validate [--ms 1000] [--json docs/validation.json]
"""

from __future__ import annotations

import argparse
import json
import logging
from pathlib import Path

import numpy as np
import pandas as pd

from . import build
from .graph import load_ordered
from .intsim import IntSim
from .model import ModelParams
from .refsim import RefSim

log = logging.getLogger(__name__)


def type_rates(types: np.ndarray, counts: np.ndarray, seconds: float) -> pd.Series:
    return pd.DataFrame({"type": types, "c": counts}).groupby("type").c.mean() / seconds


def log_rate_correlation(a: pd.Series, b: pd.Series, floor_hz: float = 1.0) -> float:
    """Pearson r of log(1 + rate) over cell types active in either run."""
    active = (a > floor_hz) | (b > floor_hz)
    if active.sum() < 4:
        return float("nan")
    return float(np.corrcoef(np.log1p(a[active]), np.log1p(b[active]))[0, 1])


def compare(graph, inp, params: ModelParams, ms: int) -> list[dict]:
    sign = build.effective_sign(inp.flags)
    types = graph.neurons["type"].fillna("(untyped)").to_numpy(dtype=object).astype(str)
    seconds = ms / 1000.0
    fine = ModelParams(**{**params.__dict__, "dt_ms": 0.1})
    rows = []
    for k, sc in enumerate(inp.scenarios):
        calc = IntSim(graph.indptr, graph.targets, graph.weights, sign, params.fixed(), seed=build.SEED)
        calc.set_stimuli([(sc.ids, sc.rate_hz)])
        calc.run(round(ms / params.dt_ms))
        refs = []
        for seed in (1, 2):
            ref = RefSim(graph.indptr, graph.targets, graph.weights, sign, fine, seed=seed)
            ref.set_stimuli([(sc.ids, sc.rate_hz)])
            ref.run(ms)
            refs.append(ref)
        r_calc = type_rates(types, calc.counts, seconds)
        r_a, r_b = (type_rates(types, r.counts, seconds) for r in refs)
        row = {
            "scenario": sc.name,
            "calc_spikes": int(calc.total_spikes),
            "ref_spikes": [int(r.counts.sum()) for r in refs],
            "calc_active": int((calc.counts > 0).sum()),
            "ref_active": [int((r.counts > 0).sum()) for r in refs],
            "r_calc_vs_ref": log_rate_correlation(r_calc, r_a),
            "r_ref_vs_ref": log_rate_correlation(r_a, r_b),
        }
        log.info(
            "%2d %-18s calc %8d spikes / %5d cells | ref %8d / %5d | r(calc,ref)=%.3f  r(ref,ref')=%.3f",
            k,
            sc.name,
            row["calc_spikes"],
            row["calc_active"],
            row["ref_spikes"][0],
            row["ref_active"][0],
            row["r_calc_vs_ref"],
            row["r_ref_vs_ref"],
        )
        rows.append(row)
    return rows


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ms", type=int, default=1000)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    graph = load_ordered()
    params = build.DEFAULT_PARAMS
    rows = compare(graph, build.from_connectome(graph, params), params, args.ms)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"ms": args.ms, "w_syn_mv": params.w_syn_mv, "scenarios": rows}, indent=2))


if __name__ == "__main__":
    main()
