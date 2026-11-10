"""Floating-point reference: the Brian2 model of Shiu et al. 2024, re-implemented
event by event so it can run on the 25.6 M-connection male CNS graph.

    dv/dt = (g - v) / tau_m   (unless refractory)      v measured from rest
    dg/dt = -g / tau_s        (unless refractory)
    threshold v > theta;  reset v = 0, g = 0;  refractory t_rfc
    synapse: g_post += sign * synapses * w_syn, t_dly after the spike
    stimulus: Poisson events kick v far above threshold; no refractory period
              for stimulated neurons

Run at dt = 0.1 ms (their setting) it is the model the calculator's 1 ms
integer simulator approximates; comparing firing rates between the two
quantifies the cost of the coarser fixed-point step.
"""

from __future__ import annotations

import numba as nb
import numpy as np

from .model import ModelParams


@nb.njit(cache=True)
def _run(
    steps,
    indptr,
    targets,
    weights,
    sign,
    v,
    g,
    refr,
    is_stim,
    ring,
    ring_n,
    ring_pos,
    stim_ids,
    stim_p,
    seed,
    counts,
    dv,
    dg,
    cv,
    theta,
    delay,
    refrac,
    kick,
):
    np.random.seed(seed)
    n = len(v)
    slots = ring.shape[0]
    total = 0
    for _ in range(steps):
        # 1. integrate non-refractory neurons
        for i in range(n):
            if refr[i] > 0:
                refr[i] -= 1
                continue
            gi = g[i]
            vi = v[i]
            if gi == 0.0 and vi == 0.0:
                continue
            v[i] = vi * dv + gi * cv
            gi *= dg
            g[i] = gi if abs(gi) > 1e-12 else 0.0
            if abs(v[i]) < 1e-12:
                v[i] = 0.0
        # 2. threshold (refractory neurons cannot spike)
        slot = ring_pos
        cnt = 0
        for i in range(n):
            if v[i] > theta and refr[i] == 0:
                ring[slot, cnt] = i
                cnt += 1
        ring_n[slot] = cnt
        # 3. synapses: spikes emitted `delay` steps ago arrive; Poisson kicks
        due = (ring_pos - delay) % slots
        for k in range(ring_n[due]):
            i = ring[due, k]
            s = float(sign[i])
            if s != 0.0:
                for j in range(indptr[i], indptr[i + 1]):
                    g[targets[j]] += s * weights[j]
        for k in range(len(stim_ids)):
            if np.random.random() < stim_p[k]:
                v[stim_ids[k]] += kick
        # 4. reset
        for k in range(cnt):
            i = ring[slot, k]
            v[i] = 0.0
            g[i] = 0.0
            if not is_stim[i]:
                refr[i] = refrac
            counts[i] += 1
        total += cnt
        ring_pos = (ring_pos + 1) % slots
    return total, ring_pos


class RefSim:
    """``sign`` is +1 / -1 / 0 per presynaptic neuron; weights are synapse counts."""

    def __init__(self, indptr, targets, weights, sign, params: ModelParams, seed: int = 1):
        self.indptr, self.targets, self.weights, self.sign = indptr, targets, weights, sign
        self.p = params
        n = len(indptr) - 1
        self.v = np.zeros(n)
        self.g = np.zeros(n)
        self.refr = np.zeros(n, np.int32)
        self.is_stim = np.zeros(n, np.bool_)
        self.counts = np.zeros(n, np.int64)
        self.delay = max(1, round(params.t_delay_ms / params.dt_ms))
        self.ring = np.zeros((self.delay + 1, n), np.int32)
        self.ring_n = np.zeros(self.delay + 1, np.int64)
        self.ring_pos = 0
        self.seed = seed
        self.stim_ids = np.zeros(0, np.int64)
        self.stim_p = np.zeros(0)

    def set_stimuli(self, stimuli) -> None:
        ids = [np.asarray(i, np.int64) for i, _ in stimuli]
        self.stim_ids = np.concatenate(ids) if ids else np.zeros(0, np.int64)
        probs = [np.full(len(i), rate * self.p.dt_ms * 1e-3) for i, (_, rate) in zip(ids, stimuli)]
        self.stim_p = np.concatenate(probs) if probs else np.zeros(0)
        self.is_stim[:] = False
        self.is_stim[self.stim_ids] = True

    def run(self, duration_ms: float) -> int:
        p = self.p
        steps = round(duration_ms / p.dt_ms)
        total, self.ring_pos = _run(
            steps,
            self.indptr,
            self.targets,
            self.weights,
            self.sign,
            self.v,
            self.g,
            self.refr,
            self.is_stim,
            self.ring,
            self.ring_n,
            self.ring_pos,
            self.stim_ids,
            self.stim_p,
            self.seed,
            self.counts,
            p.decay_v,
            p.decay_g,
            p.couple,
            p.v_thresh_mv / p.w_syn_mv,
            self.delay,
            max(1, round(p.t_refrac_ms / p.dt_ms)),
            250.0,
        )
        self.seed += 1
        return int(total)
