"""Leaky integrate-and-fire model constants.

The dynamics follow Shiu et al. 2024 ("A Drosophila computational brain model
reveals sensorimotor processing", Nature): every neuron is an identical LIF
unit with an exponentially decaying synaptic variable,

    dv/dt = (g - v) / tau_m          (v measured from rest)
    dg/dt = -g / tau_s
    presynaptic spike  ->  g_post += sign * synapse_count   (after a delay)
    v > theta          ->  spike; v = 0 and g = 0; refractory
    while refractory   ->  v and g are frozen (input still adds to g)

Stimulated neurons are forced to spike at Poisson-like times, the equivalent
of Shiu et al. kicking v far above threshold.

The calculator integrates this exactly over one fixed step in integer
arithmetic.  State is fixed point with FRAC fractional bits; one unit of g is
one synapse.  The multipliers below are Q16.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass

FRAC = 8
ONE = 1 << FRAC
Q = 16


@dataclass(frozen=True)
class ModelParams:
    dt_ms: float = 1.0  # integration step
    tau_m_ms: float = 20.0  # membrane time constant
    tau_s_ms: float = 5.0  # synaptic time constant
    v_thresh_mv: float = 7.0  # threshold above rest (-45 mV vs -52 mV)
    w_syn_mv: float = 0.275  # mV of g per synapse (free parameter)
    t_refrac_ms: float = 2.2
    t_delay_ms: float = 1.8

    @property
    def delay_steps(self) -> int:
        return max(1, round(self.t_delay_ms / self.dt_ms))

    @property
    def refrac_steps(self) -> int:
        return max(1, round(self.t_refrac_ms / self.dt_ms))

    @property
    def decay_v(self) -> float:
        return math.exp(-self.dt_ms / self.tau_m_ms)

    @property
    def decay_g(self) -> float:
        return math.exp(-self.dt_ms / self.tau_s_ms)

    @property
    def couple(self) -> float:
        """Contribution of g(t) to v(t + dt) under exact integration."""
        tm, ts = self.tau_m_ms, self.tau_s_ms
        return ts / (tm - ts) * (self.decay_v - self.decay_g)

    @property
    def v_thresh_uv(self) -> int:
        return round(self.v_thresh_mv * 1000)

    @property
    def w_syn_uv(self) -> int:
        return round(self.w_syn_mv * 1000)

    def fixed(self) -> FixedParams:
        return FixedParams(
            dv=round(self.decay_v * (1 << Q)),
            dg=round(self.decay_g * (1 << Q)),
            cv=round(self.couple * (1 << Q)),
            theta=gain_scaled(self.v_thresh_uv, self.w_syn_uv),
            delay=self.delay_steps,
            refrac=self.refrac_steps,
            dt_us=round(self.dt_ms * 1000),
        )

    def describe(self) -> dict:
        return {**asdict(self), **asdict(self.fixed())}


@dataclass(frozen=True)
class FixedParams:
    """Integer constants consumed by both the C core and the Python mirror."""

    dv: int  # Q16 membrane decay per step
    dg: int  # Q16 synaptic decay per step
    cv: int  # Q16 g -> v coupling per step
    theta: int  # spike threshold, FRAC fixed point, synapse units
    delay: int  # synaptic delay in steps
    refrac: int  # refractory period in steps
    dt_us: int  # step length in microseconds

    @property
    def timer_start(self) -> int:
        return max(self.delay, self.refrac) + 1

    @property
    def timer_deliver(self) -> int:
        return self.timer_start - self.delay + 1

    @property
    def timer_refrac_min(self) -> int:
        return self.timer_start - self.refrac + 1


def gain_scaled(microvolts: int, w_syn_uv: int) -> int:
    """microvolts expressed in synapse units, FRAC fixed point (as fs_set_gain)."""
    return (microvolts * ONE + w_syn_uv // 2) // w_syn_uv


V_FORCE = 0x40000000  # membrane value that guarantees a spike (FS_V_FORCE)


def rate_threshold(rate_hz: int, dt_us: int, dead_steps: int) -> int:
    """32-bit threshold for the per-step stimulus draw (as fs_set_stim).

    A stimulated neuron cannot fire again for ``dead_steps`` steps after a
    spike, so the per-step probability is p = r / (1 - dead * r), which makes
    the mean rate equal ``rate_hz`` until it saturates.
    """
    num = int(rate_hz) * dt_us
    dead = dead_steps * num
    if dead >= 1000000:
        return 0xFFFFFFFF
    den = 1000000 - dead
    return min(0xFFFFFFFF, ((num << 32) + den // 2) // den)
