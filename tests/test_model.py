from __future__ import annotations

import math

import numpy as np
import pytest

from pipeline.intsim import IntSim
from pipeline.model import FRAC, V_FORCE, ModelParams, gain_scaled, rate_threshold

pytestmark = pytest.mark.unit


def test_fixed_constants_match_closed_form():
    p = ModelParams()
    fx = p.fixed()
    assert fx.dv == round(math.exp(-1 / 20) * 65536)
    assert fx.dg == round(math.exp(-1 / 5) * 65536)
    assert fx.cv == round(5 / 15 * (math.exp(-1 / 20) - math.exp(-1 / 5)) * 65536)
    assert (fx.delay, fx.refrac, fx.dt_us) == (2, 2, 1000)
    assert fx.theta == gain_scaled(7000, 275) == round(7000 * 256 / 275)
    assert (fx.timer_start, fx.timer_deliver, fx.timer_refrac_min) == (3, 2, 2)


@pytest.mark.parametrize("delay_ms,refrac_ms", [(1.0, 3.0), (4.0, 1.0), (2.0, 2.0)])
def test_timer_encoding_separates_delay_and_refractoriness(delay_ms, refrac_ms):
    fx = ModelParams(t_delay_ms=delay_ms, t_refrac_ms=refrac_ms).fixed()
    assert fx.timer_start - fx.timer_deliver + 1 == fx.delay
    assert fx.timer_start - fx.timer_refrac_min + 1 == fx.refrac
    assert fx.timer_deliver >= 1 and fx.timer_refrac_min >= 1


def test_rate_threshold_compensates_for_dead_time():
    assert rate_threshold(0, 1000, 3) == 0
    assert rate_threshold(100, 1000, 0) == round(0.1 * 2**32)
    # p = r / (1 - 3 r) with r = 0.1 per step
    assert rate_threshold(100, 1000, 3) == pytest.approx(0.1 / 0.7 * 2**32, rel=1e-9)
    assert rate_threshold(250, 1000, 3) == 0xFFFFFFFF  # saturates: one spike per window
    assert rate_threshold(5000, 1000, 3) == 0xFFFFFFFF


def _chain(weight: int, sign: int = 1, n: int = 2, params: ModelParams | None = None) -> IntSim:
    """Neuron 0 -> neuron 1 with the given synapse count."""
    indptr = np.array([0, 1] + [1] * (n - 1), np.int64)
    sim = IntSim(
        indptr,
        np.array([1], np.int32),
        np.array([weight], np.int32),
        np.array([sign] + [1] * (n - 1), np.int8),
        (params or ModelParams()).fixed(),
    )
    return sim


def test_forced_event_makes_exactly_one_immediate_spike():
    sim = _chain(1)
    sim.v[0] = V_FORCE
    rec = sim.run(40, record_ids=[0])
    assert rec[:, 0].sum() == 1 and rec[0, 0]


def test_strong_synapse_relays_after_the_delay_and_weak_one_does_not():
    strong = _chain(200)
    strong.v[0] = V_FORCE
    rec = strong.run(60, record_ids=[0, 1])
    t0, t1 = int(np.argmax(rec[:, 0])), int(np.argmax(rec[:, 1]))
    assert rec[:, 1].sum() == 1 and t1 >= t0 + strong.params.delay
    weak = _chain(20)
    weak.v[0] = V_FORCE
    assert weak.run(60, record_ids=[1]).sum() == 0


def test_inhibition_and_modulatory_sign():
    inh = _chain(200, sign=-1)
    inh.v[0] = V_FORCE
    inh.run(8)
    assert inh.g[1] < 0 and inh.counts[1] == 0
    silent = _chain(200, sign=0)
    silent.v[0] = V_FORCE
    silent.run(8)
    assert silent.g[1] == 0 and silent.v[1] == 0


def test_state_decays_to_exact_zero_from_both_signs():
    for start in (1 << 20, -(1 << 20)):
        sim = _chain(1)
        sim.g[1] = start
        sim.silenced[1] = True
        sim.run(600)
        assert sim.g[1] == 0 and sim.v[1] == 0


def test_refractory_period_caps_the_rate():
    sim = _chain(1)
    for _ in range(200):
        sim.g[0] += 1 << 24  # overwhelming synaptic drive every step
        sim.run(1)
    assert sim.counts[0] == pytest.approx(200 / (sim.params.refrac + 1), abs=2)


def test_silenced_neuron_never_spikes():
    sim = _chain(1)
    sim.silenced[0] = True
    sim.g[0] = 1 << 24
    sim.set_stimuli([(np.array([0]), 200)])
    sim.run(50)
    assert sim.counts[0] == 0


def test_stimulus_rate_is_honoured_and_reproducible():
    n = 400
    indptr = np.zeros(n + 1, np.int64)
    empty = np.zeros(0, np.int32)
    runs = []
    for _ in range(2):
        sim = IntSim(indptr, empty, empty, np.ones(n, np.int8), ModelParams().fixed(), seed=99)
        sim.set_stimuli([(np.arange(n), 50)])
        sim.run(2000)
        runs.append((sim.total_spikes, sim.hash))
    assert runs[0] == runs[1]
    rate = runs[0][0] / (n * 2.0)
    assert rate == pytest.approx(50, rel=0.03)


@pytest.mark.parametrize("rate_hz", [10, 100, 200])
def test_stimulus_mean_rate_matches_request(rate_hz):
    n = 300
    sim = IntSim(
        np.zeros(n + 1, np.int64),
        np.zeros(0, np.int32),
        np.zeros(0, np.int32),
        np.ones(n, np.int8),
        ModelParams().fixed(),
        seed=5,
    )
    sim.set_stimuli([(np.arange(n), rate_hz)])
    sim.run(4000)
    assert sim.total_spikes / (n * 4.0) == pytest.approx(rate_hz, rel=0.03)


def test_threshold_scales_inversely_with_gain():
    assert ModelParams(w_syn_mv=0.1).fixed().theta > ModelParams(w_syn_mv=0.2).fixed().theta
    assert ModelParams(w_syn_mv=0.1).fixed().theta == (7000 * (1 << FRAC) + 50) // 100


def test_spike_resets_the_synaptic_variable():
    """Shiu et al.: reset is v = v_rst; g = 0."""
    sim = _chain(1)
    sim.g[0] = 1000 << FRAC  # enough to cross threshold in a single step
    sim.run(1)
    assert sim.counts[0] == 1 and sim.g[0] == 0 and sim.v[0] == 0


def test_state_is_frozen_while_refractory():
    """v and g do not evolve during the refractory period, but input still
    accumulates on g and takes effect afterwards."""
    sim = _chain(1)
    sim.v[0] = V_FORCE
    sim.run(1)  # spike; refractory for the next `refrac` steps
    sim.g[0] = 1000
    frozen = []
    for _ in range(sim.params.refrac):
        sim.run(1)
        frozen.append((int(sim.v[0]), int(sim.g[0])))
    assert frozen == [(0, 1000)] * sim.params.refrac
    sim.run(1)
    assert sim.v[0] > 0 and 0 < sim.g[0] < 1000  # integrating again


def test_threshold_is_strict():
    """Spike when v > theta, not when v == theta (Brian: 'v > v_th')."""
    for offset, spikes in ((0, 0), (1, 1)):
        sim = _chain(1)
        fx = sim.params
        # choose g so that the first integration step lands exactly on theta + offset
        target = fx.theta + offset
        g0 = (target * 65536 + fx.cv - 1) // fx.cv
        while (g0 * fx.cv) // 65536 > target:
            g0 -= 1
        assert (g0 * fx.cv) // 65536 == target
        sim.g[0] = g0
        sim.run(1)
        assert sim.counts[0] == spikes
