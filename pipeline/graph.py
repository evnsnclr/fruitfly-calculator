"""The connectome in calculator order: neurons renumbered by graph bisection,
edges in CSR form (rows = presynaptic neuron, targets ascending)."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd

from . import config, dataset, reorder


@dataclass(frozen=True)
class OrderedGraph:
    neurons: pd.DataFrame  # row i = calculator neuron index i
    indptr: np.ndarray  # int64 [n+1]
    targets: np.ndarray  # int32 [E]
    weights: np.ndarray  # int32 [E] synapse counts
    sign: np.ndarray  # int8 [n], +1 excitatory / -1 inhibitory (presynaptic)

    @property
    def n(self) -> int:
        return len(self.neurons)

    @property
    def n_edges(self) -> int:
        return len(self.targets)

    def tier(self, core: bool):
        """CSR of one weight tier: core (>= CORE_MIN_WEIGHT) or weak."""
        mask = self.weights >= config.CORE_MIN_WEIGHT
        if not core:
            mask = ~mask
        rows = np.repeat(np.arange(self.n), np.diff(self.indptr))
        counts = np.bincount(rows[mask], minlength=self.n)
        indptr = np.concatenate([[0], np.cumsum(counts)]).astype(np.int64)
        return indptr, self.targets[mask], self.weights[mask]

    def index_of_type(self, type_name: str) -> np.ndarray:
        return np.flatnonzero((self.neurons["type"] == type_name).to_numpy())


def load_ordered(use_cache: bool = True) -> OrderedGraph:
    g = dataset.load_graph(use_cache)
    perm = reorder.cached_order(g.pre, g.post, g.n) if use_cache else reorder.bisection_order(g.pre, g.post, g.n)
    order = np.argsort(perm)  # order[new] = old
    neurons = g.neurons.iloc[order].reset_index(drop=True)
    indptr, targets, weights = dataset.to_csr(
        perm[g.pre].astype(np.int32), perm[g.post].astype(np.int32), g.weight, g.n
    )
    return OrderedGraph(neurons, indptr, targets, weights, neurons["sign"].to_numpy(np.int8))
