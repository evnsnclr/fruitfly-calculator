"""Tools that are not on the calculator: ordering, the floating-point
reference model, and the reporting helpers."""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from pipeline import reorder, report, validate
from pipeline.dataset import to_csr
from pipeline.intsim import IntSim
from pipeline.model import ModelParams
from pipeline.refsim import RefSim

pytestmark = pytest.mark.unit


def clustered_edges(n: int = 400, clusters: int = 8, seed: int = 0):
    """Neurons belong to hidden clusters and mostly target their own cluster,
    but their indices are shuffled - the structure bisection should recover."""
    rng = np.random.default_rng(seed)
    label = rng.permutation(n) % clusters
    members = [np.flatnonzero(label == c) for c in range(clusters)]
    pre, post = [], []
    for i in range(n):
        own = rng.choice(members[label[i]], size=12, replace=False)
        other = rng.integers(0, n, size=2)
        tgt = np.unique(np.concatenate([own, other]))
        pre.append(np.full(len(tgt), i))
        post.append(tgt)
    return np.concatenate(pre).astype(np.int32), np.concatenate(post).astype(np.int32)


def mean_log_gap(pre, post, n) -> float:
    indptr, targets, _ = to_csr(pre, post, np.ones(len(pre), np.int32), n)
    total = 0.0
    for r in range(n):
        row = targets[indptr[r] : indptr[r + 1]].astype(np.int64)
        total += np.log2(np.diff(np.concatenate([[-1], row]))).sum()
    return total / len(targets)


def test_bisection_order_is_a_deterministic_permutation():
    pre, post = clustered_edges()
    perm = reorder.bisection_order(pre, post, 400)
    assert sorted(perm.tolist()) == list(range(400))
    assert np.array_equal(perm, reorder.bisection_order(pre, post, 400))
    assert reorder.edge_digest(pre, post, 400) != reorder.edge_digest(post, pre, 400)


def test_bisection_order_makes_target_gaps_smaller():
    pre, post = clustered_edges()
    perm = reorder.bisection_order(pre, post, 400)
    before = mean_log_gap(pre, post, 400)
    after = mean_log_gap(perm[pre].astype(np.int32), perm[post].astype(np.int32), 400)
    assert after < before - 0.5, (before, after)


@pytest.mark.parametrize("n", [0, 1, 5])
def test_bisection_order_handles_tiny_graphs(n):
    empty = np.zeros(0, np.int32)
    assert sorted(reorder.bisection_order(empty, empty, n).tolist()) == list(range(n))


def _feedforward(n_in: int = 40, n_out: int = 40, weight: int = 6, seed: int = 2):
    """Every input neuron drives every output neuron with a few synapses."""
    n = n_in + n_out
    pre = np.repeat(np.arange(n_in), n_out).astype(np.int32)
    post = np.tile(np.arange(n_in, n), n_in).astype(np.int32)
    indptr, targets, weights = to_csr(pre, post, np.full(len(pre), weight, np.int32), n)
    return n, indptr, targets, weights


def test_reference_model_agrees_with_the_integer_model_on_rates():
    """Different step size, arithmetic and random numbers: the two models
    should still give the same firing rates on a simple driven network."""
    n, indptr, targets, weights = _feedforward()
    sign = np.ones(n, np.int8)
    params = ModelParams()
    stim = np.arange(40)
    calc = IntSim(indptr, targets, weights, sign, params.fixed(), seed=3)
    calc.set_stimuli([(stim, 40)])
    calc.run(2000)
    ref = RefSim(indptr, targets, weights, sign, ModelParams(dt_ms=0.1), seed=3)
    ref.set_stimuli([(stim, 40)])
    ref.run(2000)
    in_calc, in_ref = calc.counts[:40].mean() / 2, ref.counts[:40].mean() / 2
    out_calc, out_ref = calc.counts[40:].mean() / 2, ref.counts[40:].mean() / 2
    assert in_calc == pytest.approx(40, rel=0.1) and in_ref == pytest.approx(40, rel=0.1)
    assert out_calc > 5 and out_ref > 5, "the test network should relay activity"
    assert out_calc == pytest.approx(out_ref, rel=0.25)


def test_reference_model_inhibition_and_silence():
    n, indptr, targets, weights = _feedforward()
    stim = np.arange(40)
    quiet = RefSim(indptr, targets, weights, np.ones(n, np.int8), ModelParams(dt_ms=0.1))
    assert quiet.run(50) == 0  # no stimulus, no spontaneous activity
    inhibited = RefSim(indptr, targets, weights, -np.ones(n, np.int8), ModelParams(dt_ms=0.1))
    inhibited.set_stimuli([(stim, 100)])
    inhibited.run(300)
    assert inhibited.counts[40:].sum() == 0 and inhibited.counts[:40].sum() > 0
    assert (inhibited.g[40:] <= 0).all()


def test_rate_helpers():
    types = np.array(["a", "a", "b", "c", "c", "c", "d", "e"])
    counts = np.array([10, 30, 0, 3, 3, 3, 100, 7])
    rates = validate.type_rates(types, counts, seconds=2.0)
    assert rates.to_dict() == {"a": 10.0, "b": 0.0, "c": 1.5, "d": 50.0, "e": 3.5}
    assert validate.log_rate_correlation(rates, rates) == pytest.approx(1.0)
    assert validate.log_rate_correlation(rates, rates[::-1].set_axis(rates.index)) < 1.0
    quiet = pd.Series({"a": 0.0, "b": 0.5})
    assert np.isnan(validate.log_rate_correlation(quiet, quiet))


def test_report_row_formatting():
    from pipeline.groups import GROUPS

    groups = {g.name: 0.0 for g in GROUPS[1:]}
    groups["Proboscis"] = 42.4
    groups["Brain"] = 99.0  # not a motor pool: must not appear in the motor list
    row = {
        "index": 3,
        "name": "demo",
        "hint": "",
        "cells": 2,
        "rate_hz": 100,
        "seed": 1,
        "spikes": 1234,
        "active": 56,
        "groups": groups,
        "top": [("typeA", 80.2), ("typeB", 12.0)],
    }
    text = report.format_row(row)
    assert "demo" in text and "1,234" in text and "Proboscis 42" in text and "Brain" not in text
    assert "typeA 80, typeB 12" in text
