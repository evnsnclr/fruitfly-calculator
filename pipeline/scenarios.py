"""Built-in experiments: which neurons to drive, how hard, and what to watch.

Each scenario activates a named population with Poisson input, the software
analogue of optogenetic activation or of presenting a stimulus to the sensory
neurons listed.  The populations are picked by cell-type annotation only;
everything downstream is whatever the connectome does with it.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd

from .groups import neuron_side


@dataclass(frozen=True)
class Scenario:
    name: str  # <= 19 chars
    hint: str  # <= 22 chars: the circuit to watch for
    rate_hz: int
    types: tuple[str, ...] = ()
    prefixes: tuple[str, ...] = ()
    subclasses: tuple[str, ...] = ()


SCENARIOS: tuple[Scenario, ...] = (
    Scenario("Sugar on tongue", "taste > proboscis out", 150, ("LB3a", "LB3b", "LB3c", "LB3d")),
    Scenario("Looming shadow", "LPLC2/LC4 > GF > jump", 50, ("LPLC2", "LC4")),
    Scenario("Courtship P1", "P1 > pIP10 > wing song", 50, prefixes=("pC1",)),
    Scenario("Song cmd pIP10", "pIP10 > wing muscles", 100, ("pIP10",)),
    Scenario("Lick cmd aSP22", "aSP22 > proboscis", 100, ("aSP22",)),
    Scenario("Moonwalker MDN", "MDN > LBL40 > legs", 100, ("MDN",)),
    Scenario("Groom cmd aDN1", "aDN1 > legs + antenna", 100, ("DNg62",)),
    Scenario("Dust on eyes", "eye bristles > groom", 80, ("BM_InOm",)),
    Scenario("Optic flow", "T4a/T5a > HS cells", 30, ("T4a", "T5a")),
    Scenario("Compass EPG", "EPG > ring neurons", 50, ("EPG",)),
    Scenario("Chase LC10a", "LC10a > AOTU019", 50, ("LC10a",)),
    Scenario("Female pheromone", "leg ppk25 > PPN1/vAB3", 50, ("LgLG1b", "LgLG5")),
    Scenario("Wing load sensors", "campaniform > b1 MN", 50, subclasses=("campaniform sensilla",)),
    Scenario("Bitter taste", "bitter GRNs > Scapula", 60, prefixes=("LB1",)),
    Scenario("Sound (JO-A/B)", "Johnston organ > AMMC", 50, prefixes=("JO-A", "JO-B")),
    Scenario("Giant fiber", "DNp01 > TTMn (jump)", 200, ("DNp01",)),
)


def scenario_ids(sc: Scenario, neurons: pd.DataFrame) -> tuple[np.ndarray, int, int]:
    """Neuron indices ordered left, right, unsided; plus the two side counts."""
    typ = neurons["type"].fillna("").to_numpy(dtype=object).astype(str)
    mask = np.isin(typ, sc.types)
    for p in sc.prefixes:
        mask |= np.char.startswith(typ, p)
    if sc.subclasses:
        mask |= neurons["subclass"].isin(sc.subclasses).to_numpy(dtype=bool)
    side = neuron_side(neurons)
    left = np.flatnonzero(mask & (side == "L"))
    right = np.flatnonzero(mask & (side == "R"))
    rest = np.flatnonzero(mask & (side == ""))
    return np.concatenate([left, right, rest]).astype(np.uint32), len(left), len(right)
