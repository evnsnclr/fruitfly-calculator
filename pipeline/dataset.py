"""Load the male CNS flat connectome and reduce it to a neuron-to-neuron graph.

Neuron set: every body with a non-null ``superclass`` annotation (166,700 in
v1.0).  Edges: every body_pre -> body_post row whose two ends are both in the
neuron set, with the synapse count as weight.  No weight threshold is applied.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd
from pyarrow import feather

from . import config

# Sign of the postsynaptic effect per predicted transmitter.  Acetylcholine is
# excitatory; GABA and glutamate are treated as inhibitory (as in Shiu et al.
# 2024); histamine (photoreceptors) opens chloride channels, so inhibitory.
# Aminergic/unknown predictions default to excitatory, again following Shiu.
INHIBITORY_NT = frozenset({"gaba", "glutamate", "histamine"})


@dataclass(frozen=True)
class Graph:
    """Neuron table plus COO edge list, both in sorted-bodyId order."""

    neurons: pd.DataFrame  # one row per neuron, index 0..n-1, sorted by bodyId
    pre: np.ndarray  # int32 [E] presynaptic neuron index
    post: np.ndarray  # int32 [E] postsynaptic neuron index
    weight: np.ndarray  # int32 [E] synapse count (>= 1)

    @property
    def n(self) -> int:
        return len(self.neurons)

    @property
    def n_edges(self) -> int:
        return len(self.weight)


def load_neurons() -> pd.DataFrame:
    ann = feather.read_feather(config.RAW / config.ANNOTATIONS)
    neu = ann[ann["superclass"].notna()].sort_values("bodyId").reset_index(drop=True)
    if neu["bodyId"].duplicated().any():
        raise ValueError("duplicate bodyId in annotation table")
    nt = feather.read_feather(config.RAW / config.NEUROTRANSMITTERS)
    nt = nt.drop_duplicates("body").set_index("body")
    neu["nt"] = nt["consensus_nt"].reindex(neu["bodyId"]).to_numpy()
    neu["nt"] = neu["nt"].fillna("unknown")
    neu["sign"] = np.where(neu["nt"].isin(INHIBITORY_NT), -1, 1).astype(np.int8)
    return neu


def source_stamp() -> np.ndarray:
    """Size and mtime of the three source tables; caches are tied to it."""
    stats = [(config.RAW / name).stat() for name in (config.ANNOTATIONS, config.NEUROTRANSMITTERS, config.WEIGHTS)]
    return np.array([[st.st_size, st.st_mtime_ns] for st in stats], np.int64)


def load_graph(use_cache: bool = True) -> Graph:
    """Return the neuron-to-neuron graph; cached under data/cache."""
    config.CACHE.mkdir(parents=True, exist_ok=True)
    cache = config.CACHE / "graph_coo.npz"
    neurons = load_neurons()
    ids = neurons["bodyId"].to_numpy()
    stamp = source_stamp()
    if use_cache and cache.exists():
        d = np.load(cache)
        if "stamp" in d and np.array_equal(d["stamp"], stamp) and np.array_equal(d["ids"], ids):
            return Graph(neurons, d["pre"], d["post"], d["weight"])
    table = feather.read_table(config.RAW / config.WEIGHTS)
    pre = table["body_pre"].to_numpy()
    post = table["body_post"].to_numpy()
    weight = table["weight"].to_numpy()
    ip = np.searchsorted(ids, pre)
    ip[ip >= len(ids)] = 0
    iq = np.searchsorted(ids, post)
    iq[iq >= len(ids)] = 0
    keep = (ids[ip] == pre) & (ids[iq] == post) & (weight > 0)
    g = Graph(neurons, ip[keep].astype(np.int32), iq[keep].astype(np.int32), weight[keep].astype(np.int32))
    np.savez(cache, ids=ids, pre=g.pre, post=g.post, weight=g.weight, stamp=stamp)
    return g


def to_csr(pre: np.ndarray, post: np.ndarray, weight: np.ndarray, n: int):
    """Sort a COO edge list into CSR (rows = presynaptic, columns ascending)."""
    order = np.lexsort((post, pre))
    indptr = np.zeros(n + 1, np.int64)
    np.add.at(indptr, pre.astype(np.int64) + 1, 1)
    np.cumsum(indptr, out=indptr)
    return indptr, post[order].astype(np.int32), weight[order].astype(np.int32)
