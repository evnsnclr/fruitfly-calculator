"""Run every built-in scenario on the host and summarise what the model does.

python -m pipeline.report [--ms 1000] [--w-syn 0.13] [--markdown docs/scenarios.md]
"""

from __future__ import annotations

import argparse
import logging

import numpy as np
import pandas as pd

from . import build
from .graph import load_ordered
from .groups import GROUPS
from .intsim import IntSim
from .model import ModelParams

log = logging.getLogger(__name__)


def run_scenarios(graph, inp, params: ModelParams, ms: int, seeds=(build.SEED,)) -> list[dict]:
    sign = build.effective_sign(inp.flags)
    typ = graph.neurons["type"].fillna("(untyped)").to_numpy(dtype=object).astype(str)
    sizes = np.bincount(inp.group_ids, minlength=len(GROUPS))
    out = []
    for k, sc in enumerate(inp.scenarios):
        for seed in seeds:
            sim = IntSim(graph.indptr, graph.targets, graph.weights, sign, params.fixed(), seed=seed)
            sim.set_stimuli([(sc.ids, sc.rate_hz)])
            sim.run(round(ms / params.dt_ms))
            secs = ms / 1000.0
            grp = np.bincount(inp.group_ids, weights=sim.counts, minlength=len(GROUPS)) / np.maximum(sizes, 1) / secs
            driven = np.zeros(graph.n, bool)
            driven[sc.ids] = True
            rates = pd.DataFrame({"type": typ[~driven], "c": sim.counts[~driven]}).groupby("type").c.mean() / secs
            top = rates[rates >= 5].sort_values(ascending=False).head(10)
            out.append(
                {
                    "index": k,
                    "name": sc.name,
                    "hint": sc.hint,
                    "cells": len(sc.ids),
                    "rate_hz": sc.rate_hz,
                    "seed": seed,
                    "spikes": sim.total_spikes,
                    "active": int((sim.counts > 0).sum()),
                    "groups": {GROUPS[g].name: float(grp[g]) for g in range(1, len(GROUPS))},
                    "top": [(t, float(r)) for t, r in top.items()],
                }
            )
    return out


def format_row(r: dict) -> str:
    motor = ", ".join(
        f"{n} {v:.0f}" for n, v in r["groups"].items() if v >= 1.0 and GROUPS[[g.name for g in GROUPS].index(n)].motor
    )
    top = ", ".join(f"{t} {v:.0f}" for t, v in r["top"][:8])
    return (
        f"{r['index']:2d} {r['name']:18s} {r['cells']:4d}@{r['rate_hz']:<3d} spikes {r['spikes']:>8,} active {r['active']:>5}"
        f"\n     motor Hz: {motor or '-'}\n     top  Hz: {top or '-'}"
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ms", type=int, default=1000)
    ap.add_argument("--w-syn", type=float, default=build.DEFAULT_PARAMS.w_syn_mv)
    ap.add_argument("--seeds", type=int, default=1)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    graph = load_ordered()
    params = ModelParams(w_syn_mv=args.w_syn)
    inp = build.from_connectome(graph, params)
    seeds = tuple(build.SEED + 7919 * i for i in range(args.seeds))
    for r in run_scenarios(graph, inp, params, args.ms, seeds):
        log.info(format_row(r))


if __name__ == "__main__":
    main()
