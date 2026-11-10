"""Build the calculator data set from the male CNS connectome.

python -m pipeline.build [--out build/calc] [--w-syn 0.13]
"""

from __future__ import annotations

import argparse
import logging
from pathlib import Path

import numpy as np

from . import config, layout, pack
from .aliases import type_aliases
from .graph import OrderedGraph, load_ordered
from .groups import GROUPS, assign_groups, neuron_side
from .intsim import IntSim
from .model import ModelParams
from .scenarios import SCENARIOS, scenario_ids

log = logging.getLogger(__name__)

# Predicted transmitters that act through slow metabotropic receptors: such
# neurons spike but have no fast synaptic effect in this model.
MODULATORY_NT = frozenset({"dopamine", "octopamine", "serotonin"})
# No consensus transmitter prediction.  The sign is not guessed: these neurons
# get no synaptic effect, except sensory afferents, which in insects are
# cholinergic (photoreceptors, the histaminergic exception, are predicted).
UNDETERMINED_NT = frozenset({"unclear", "unknown"})
UNTYPED = "(untyped)"
# Free parameter of the model.  Shiu et al. used 0.275 mV on FlyWire; the male
# CNS reconstruction has about twice the synapses per neuron, and 0.2 mV keeps
# it below runaway for every built-in scenario (see docs).
DEFAULT_PARAMS = ModelParams(w_syn_mv=0.2)
SEED = 0x1234ABCD
# (scenario index, steps) run by the calculator at boot and compared with
# the spike-train hash computed here by the Python mirror.
SELFTEST_PROTOCOL = ((3, 200), (1, 60))


def neuron_flags(graph: OrderedGraph) -> np.ndarray:
    nt = graph.neurons["nt"].to_numpy(dtype=object)
    sensory = graph.neurons["superclass"].str.contains("sensory").to_numpy(dtype=bool)
    sign = np.where(graph.sign < 0, pack.SIGN_INH, pack.SIGN_EXC).astype(np.uint8)
    sign[np.isin(nt, list(MODULATORY_NT))] = pack.SIGN_NONE
    sign[np.isin(nt, list(UNDETERMINED_NT)) & ~sensory] = pack.SIGN_NONE
    side = np.array([pack.SIDE_CODE[s] for s in neuron_side(graph.neurons)], np.uint8)
    return sign | (side << pack.SIDE_SHIFT)


def effective_sign(flags: np.ndarray) -> np.ndarray:
    """int8 sign per neuron (+1, -1, 0) as the simulators apply it."""
    code = flags & 3
    return np.select([code == pack.SIGN_INH, code == pack.SIGN_NONE], [-1, 0], 1).astype(np.int8)


def type_table(graph: OrderedGraph) -> tuple[np.ndarray, tuple[str, ...]]:
    typ = graph.neurons["type"].fillna(UNTYPED).to_numpy(dtype=object).astype(str)
    names = sorted(set(typ) - {UNTYPED}, key=lambda s: (s.lower(), s))
    names = (UNTYPED, *names)
    index = {name: i for i, name in enumerate(names)}
    return np.array([index[t] for t in typ], np.uint16), names


def reference_selftests(
    graph: OrderedGraph, flags: np.ndarray, scenarios, params: ModelParams, protocol=SELFTEST_PROTOCOL, seed: int = SEED
) -> tuple[tuple[int, int, int, int], ...]:
    """Expected (spikes, hash) for each self-test, from the Python mirror."""
    sign = effective_sign(flags)
    out = []
    for scenario, steps in protocol:
        sim = IntSim(graph.indptr, graph.targets, graph.weights, sign, params.fixed(), seed=seed)
        sim.set_stimuli([(scenarios[scenario].ids, scenarios[scenario].rate_hz)])
        sim.run(steps)
        out.append((scenario, steps, sim.total_spikes, sim.hash))
    return tuple(out)


def scenario_usage(
    graph: OrderedGraph, flags: np.ndarray, scenarios, params: ModelParams, ms: int = 600, seed: int = SEED
) -> np.ndarray:
    """Per-neuron share of the spikes fired across the built-in scenarios
    (each scenario weighted equally)."""
    sign = effective_sign(flags)
    usage = np.zeros(graph.n)
    for sc in scenarios:
        sim = IntSim(graph.indptr, graph.targets, graph.weights, sign, params.fixed(), seed=seed)
        sim.set_stimuli([(sc.ids, sc.rate_hz)])
        sim.run(round(ms / params.dt_ms))
        usage += sim.counts / max(1, sim.total_spikes)
    return usage


def from_connectome(graph: OrderedGraph, params: ModelParams = DEFAULT_PARAMS) -> pack.PackInput:
    lay = layout.compute_layout(graph.neurons, graph.indptr, graph.targets, graph.weights)
    type_ids, type_names = type_table(graph)
    flags = neuron_flags(graph)
    scenarios = []
    for sc in SCENARIOS:
        ids, n_left, n_right = scenario_ids(sc, graph.neurons)
        if len(ids) == 0:
            raise ValueError(f"scenario {sc.name!r} matches no neurons")
        scenarios.append(pack.ScenarioInfo(sc.name, sc.hint, sc.rate_hz, ids, n_left, n_right))
    return pack.PackInput(
        indptr=graph.indptr,
        targets=graph.targets,
        weights=graph.weights,
        flags=flags,
        pix=lay.pix,
        type_ids=type_ids,
        type_names=type_names,
        type_aliases=type_aliases(graph.neurons, type_names),
        group_ids=assign_groups(graph.neurons),
        groups=tuple(pack.GroupInfo(g.name, g.rgb, g.motor) for g in GROUPS),
        scenarios=tuple(scenarios),
        params=params,
        map_w=layout.MAP_W,
        map_h=layout.MAP_H,
        seed=SEED,
        selftests=reference_selftests(graph, flags, scenarios, params),
        usage=scenario_usage(graph, flags, scenarios, params),
        extra={"dataset": config.DATASET},
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=config.OUT)
    ap.add_argument("--w-syn", type=float, default=DEFAULT_PARAMS.w_syn_mv, help="mV of g per synapse")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    graph = load_ordered()
    log.info(
        "graph: %s neurons, %s connections, %s synapses",
        f"{graph.n:,}",
        f"{graph.n_edges:,}",
        f"{int(graph.weights.sum()):,}",
    )
    manifest = pack.write_pack(from_connectome(graph, ModelParams(w_syn_mv=args.w_syn)), args.out)
    total = manifest["meta"]["bytes"] + sum(t["bytes"] for t in manifest["tiers"])
    log.info("wrote %s: %.2f MB total", args.out, total / 1e6)


if __name__ == "__main__":
    main()
