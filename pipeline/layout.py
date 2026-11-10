"""Screen layout: one pixel per neuron on a 180 x 240 map.

The brain is drawn in frontal view (soma x/y) and the ventral nerve cord
below it in dorsal view (soma x/z), both at the same scale, the way a
dissected fly CNS is usually sketched.  Neurons without a soma in the volume
(sensory axons, mostly) are placed at the synapse-weighted centroid of their
partners' somata.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd

MAP_W = 180
MAP_H = 240
BRAIN_VNC_SPLIT_Z = 52000.0  # voxels (8 nm); somata beyond this are in the VNC
MARGIN = 2
GAP_ROWS = 3


@dataclass(frozen=True)
class Layout:
    x: np.ndarray  # int16 [n] pixel column
    y: np.ndarray  # int16 [n] pixel row
    in_vnc: np.ndarray  # bool [n]

    @property
    def pix(self) -> np.ndarray:
        return (self.y.astype(np.int64) * MAP_W + self.x).astype(np.uint16)


def soma_xyz(neurons: pd.DataFrame) -> np.ndarray:
    """float [n, 3] soma coordinates, NaN where unknown."""
    out = np.full((len(neurons), 3), np.nan)
    for i, loc in enumerate(neurons["somaLocation"].to_numpy()):
        if loc is not None and len(loc) == 3:
            out[i] = loc
    return out


def fill_missing_positions(
    xyz: np.ndarray, indptr: np.ndarray, targets: np.ndarray, weights: np.ndarray, rounds: int = 3
) -> np.ndarray:
    """Give soma-less neurons the weighted centroid of their partners."""
    xyz = xyz.copy()
    n = len(xyz)
    rows = np.repeat(np.arange(n), np.diff(indptr))
    for _ in range(rounds):
        known = ~np.isnan(xyz[:, 0])
        if known.all():
            break
        acc = np.zeros((n, 3))
        wsum = np.zeros(n)
        for src, dst in ((rows, targets), (targets, rows)):
            use = known[dst] & ~known[src]
            w = weights[use].astype(float)
            np.add.at(acc, src[use], xyz[dst[use]] * w[:, None])
            np.add.at(wsum, src[use], w)
        got = (~known) & (wsum > 0)
        xyz[got] = acc[got] / wsum[got, None]
    still = np.isnan(xyz[:, 0])
    if still.any():
        xyz[still] = np.nanmedian(xyz, axis=0)
    return xyz


def compute_layout(
    neurons: pd.DataFrame, indptr: np.ndarray, targets: np.ndarray, weights: np.ndarray, seed: int = 7
) -> Layout:
    raw = soma_xyz(neurons)
    had_soma = ~np.isnan(raw[:, 0])
    xyz = fill_missing_positions(raw, indptr, targets, weights)
    rng = np.random.default_rng(seed)
    # Soma-less neurons would pile onto a few pixels; spread them a little.
    jitter = rng.normal(0.0, 900.0, size=xyz.shape)
    xyz = np.where(had_soma[:, None], xyz, xyz + jitter)

    in_vnc = xyz[:, 2] > BRAIN_VNC_SPLIT_Z
    brain = ~in_vnc
    # Robust extents so a few stray points do not shrink everything.
    bx0, bx1 = np.percentile(xyz[brain, 0], [0.05, 99.95])
    by0, by1 = np.percentile(xyz[brain, 1], [0.05, 99.95])
    vx0, vx1 = np.percentile(xyz[in_vnc, 0], [0.05, 99.95])
    vz0, vz1 = np.percentile(xyz[in_vnc, 2], [0.05, 99.95])
    usable_w = MAP_W - 2 * MARGIN
    usable_h = MAP_H - 2 * MARGIN - GAP_ROWS
    scale = max((bx1 - bx0) / usable_w, ((by1 - by0) + (vz1 - vz0)) / usable_h)
    brain_h = (by1 - by0) / scale
    cx = MAP_W / 2.0
    mid_x = (bx0 + bx1) / 2.0

    px = np.empty(len(xyz))
    py = np.empty(len(xyz))
    px[brain] = cx + (xyz[brain, 0] - mid_x) / scale
    py[brain] = MARGIN + (xyz[brain, 1] - by0) / scale
    px[in_vnc] = cx + (xyz[in_vnc, 0] - (vx0 + vx1) / 2.0) / scale
    py[in_vnc] = MARGIN + brain_h + GAP_ROWS + (xyz[in_vnc, 2] - vz0) / scale
    x = np.clip(np.rint(px), 0, MAP_W - 1).astype(np.int16)
    y = np.clip(np.rint(py), 0, MAP_H - 1).astype(np.int16)
    return Layout(x, y, in_vnc)
