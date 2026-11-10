"""Integer LIF simulator: a line-for-line Python mirror of core/flysim.c.

Used to (a) calibrate and explore the model on the host and (b) check that the
C core is bit-exact.  All arithmetic is int32/int64 two's complement, so the
calculator, the host C build and this module produce identical spike trains.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numba as nb
import numpy as np

from .model import FRAC, V_FORCE, FixedParams, rate_threshold


@nb.njit(cache=True, inline="always")
def _xorshift32(x):
    x ^= (x << np.uint32(13)) & np.uint32(0xFFFFFFFF)
    x ^= x >> np.uint32(17)
    x ^= (x << np.uint32(5)) & np.uint32(0xFFFFFFFF)
    return x


@nb.njit(cache=True, inline="always")
def _mulq16(a, k):
    """(a * k) / 2**16 rounded toward zero, like C integer division."""
    y = np.int64(a) * np.int64(k)
    if y < 0:
        return -((-y) >> np.int64(16))
    return y >> np.int64(16)


@nb.njit(cache=True, inline="always")
def _wrap32(x):
    """Reduce an int64 to int32 two's complement, as the C core's adds do."""
    return ((np.int64(x) + np.int64(0x80000000)) & np.int64(0xFFFFFFFF)) - np.int64(0x80000000)


@nb.njit(cache=True)
def _run(
    steps,
    indptr,
    targets,
    weights,
    sign,
    v,
    g,
    tmr,
    silenced,
    stim_ptr,
    stim_ids,
    stim_thresh,
    rng_state,
    counts,
    dv,
    dg,
    cv,
    theta,
    timer_start,
    timer_deliver,
    timer_refrac_min,
    v_force,
    record_ids,
    record_out,
    step0,
    hash_state,
):
    n = len(v)
    rng = np.uint32(rng_state)
    h = np.uint64(hash_state)
    mask32 = np.uint64(0xFFFFFFFF)
    prime = np.uint64(16777619)
    total = 0
    n_rec = len(record_ids)
    for step in range(steps):
        # Pass A: deliver spikes whose synaptic delay has elapsed.
        for i in range(n):
            if tmr[i] == timer_deliver:
                s = np.int32(sign[i]) << FRAC
                if s != 0:
                    for j in range(indptr[i], indptr[i + 1]):
                        g[targets[j]] += s * weights[j]
        # Stimulus: one RNG draw per stimulated neuron per step; an event
        # forces a spike unless the previous spike's timer is still running.
        for k in range(len(stim_thresh)):
            th = stim_thresh[k]
            for j in range(stim_ptr[k], stim_ptr[k + 1]):
                rng = _xorshift32(rng)
                if rng < th:
                    i = stim_ids[j]
                    if tmr[i] == 0 and not silenced[i]:
                        v[i] = v_force
        # Pass B: integrate, detect threshold crossings.
        for i in range(n):
            t = tmr[i]
            gi = g[i]
            vi = v[i]
            if t == 0 and gi == 0 and vi == 0:
                continue
            refractory = False
            if t != 0:
                refractory = t >= timer_refrac_min
                tmr[i] = t - 1
            if silenced[i]:
                v[i] = 0
                g[i] = _mulq16(gi, dg)
            elif refractory:
                pass  # v and g frozen; arriving input keeps adding to g
            else:
                vn = _wrap32(_mulq16(vi, dv) + _mulq16(gi, cv))
                if vn > theta:
                    v[i] = 0
                    g[i] = 0
                    tmr[i] = timer_start
                    counts[i] += 1
                    total += 1
                    h = ((h ^ np.uint64(i)) * prime) & mask32
                    h = ((h ^ np.uint64(step0 + step)) * prime) & mask32
                else:
                    v[i] = vn
                    g[i] = _mulq16(gi, dg)
        for r in range(n_rec):
            record_out[step, r] = tmr[record_ids[r]] == timer_start
    return rng, total, h


@dataclass
class IntSim:
    """Stateful wrapper.  ``stimuli`` is an ordered list of (ids, rate_hz)."""

    indptr: np.ndarray
    targets: np.ndarray
    weights: np.ndarray
    sign: np.ndarray
    params: FixedParams
    seed: int = 0x1234ABCD
    stimuli: list = field(default_factory=list)

    def __post_init__(self):
        n = len(self.indptr) - 1
        self.n = n
        self.v = np.zeros(n, np.int32)
        self.g = np.zeros(n, np.int32)
        self.tmr = np.zeros(n, np.uint8)
        self.silenced = np.zeros(n, np.bool_)
        self.counts = np.zeros(n, np.int64)
        self.rng = np.uint32(self.seed)
        self.steps = 0
        self.total_spikes = 0
        self.hash = 2166136261  # FNV-1a offset basis, as fs_reset

    def set_stimuli(self, stimuli):
        self.stimuli = [(np.asarray(ids, np.int32), int(rate)) for ids, rate in stimuli]

    def run(self, steps: int, record_ids=None):
        p = self.params
        ptr = np.zeros(len(self.stimuli) + 1, np.int64)
        for k, (ids, _) in enumerate(self.stimuli):
            ptr[k + 1] = ptr[k] + len(ids)
        ids = np.concatenate([s[0] for s in self.stimuli]) if self.stimuli else np.zeros(0, np.int32)
        thresh = np.array([rate_threshold(rate, p.dt_us, p.timer_start) for _, rate in self.stimuli], np.uint32)
        rec_ids = np.asarray(record_ids if record_ids is not None else [], np.int64)
        rec = np.zeros((steps, len(rec_ids)), np.bool_)
        self.rng, total, h = _run(
            steps,
            self.indptr,
            self.targets,
            self.weights,
            self.sign,
            self.v,
            self.g,
            self.tmr,
            self.silenced,
            ptr,
            ids.astype(np.int32),
            thresh,
            self.rng,
            self.counts,
            p.dv,
            p.dg,
            p.cv,
            p.theta,
            p.timer_start,
            p.timer_deliver,
            p.timer_refrac_min,
            V_FORCE,
            rec_ids,
            rec,
            self.steps,
            self.hash,
        )
        self.hash = int(h)
        self.steps += steps
        self.total_spikes += int(total)
        return rec
