"""Neuron ordering by recursive graph bisection.

Renumbering neurons so that each neuron's postsynaptic targets sit close
together is what makes the connectome compressible (small target gaps) and
cache-friendly on the calculator.  This is the "BP" algorithm of Dhulipala et
al., "Compressing Graphs and Indexes with Recursive Graph Bisection" (KDD
2016): recursively split the vertex set in two and swap vertices between the
halves while that lowers an estimate of the log-gap coding cost.
"""

from __future__ import annotations

import hashlib

import numba as nb
import numpy as np

from . import config


@nb.njit(cache=True)
def _move_gain(d1, d2, l1, l2, log2t, from_first):
    before = d1 * (l1 - log2t[d1 + 1]) + d2 * (l2 - log2t[d2 + 1])
    if from_first:
        after = (d1 - 1) * (l1 - log2t[d1]) + (d2 + 1) * (l2 - log2t[d2 + 2])
    else:
        after = (d1 + 1) * (l1 - log2t[d1 + 2]) + (d2 - 1) * (l2 - log2t[d2])
    return before - after


@nb.njit(cache=True)
def _bisect(order, lo, hi, in_ptr, in_adj, side, deg1, deg2, gain, log2t, iters):
    mid = (lo + hi) // 2
    n1 = mid - lo
    n2 = hi - mid
    for i in range(lo, mid):
        side[order[i]] = 0
    for i in range(mid, hi):
        side[order[i]] = 1
    l1 = np.log2(n1)
    l2 = np.log2(n2)
    for _ in range(iters):
        for i in range(lo, hi):
            v = order[i]
            for j in range(in_ptr[v], in_ptr[v + 1]):
                q = in_adj[j]
                deg1[q] = 0
                deg2[q] = 0
        for i in range(lo, hi):
            v = order[i]
            if side[v] == 0:
                for j in range(in_ptr[v], in_ptr[v + 1]):
                    deg1[in_adj[j]] += 1
            else:
                for j in range(in_ptr[v], in_ptr[v + 1]):
                    deg2[in_adj[j]] += 1
        for i in range(lo, hi):
            v = order[i]
            g = 0.0
            first = side[v] == 0
            for j in range(in_ptr[v], in_ptr[v + 1]):
                q = in_adj[j]
                g += _move_gain(deg1[q], deg2[q], l1, l2, log2t, first)
            gain[v] = g
        left = order[lo:mid].copy()
        right = order[mid:hi].copy()
        gl = np.empty(n1)
        gr = np.empty(n2)
        for i in range(n1):
            gl[i] = -gain[left[i]]
        for i in range(n2):
            gr[i] = -gain[right[i]]
        il = np.argsort(gl, kind="mergesort")
        ir = np.argsort(gr, kind="mergesort")
        swaps = 0
        for i in range(min(n1, n2)):
            a = left[il[i]]
            b = right[ir[i]]
            if gain[a] + gain[b] <= 0:
                break
            left[il[i]] = b
            right[ir[i]] = a
            side[a] = 1
            side[b] = 0
            swaps += 1
        order[lo:mid] = left
        order[mid:hi] = right
        if swaps == 0:
            break


@nb.njit(cache=True)
def _bp_order(n, in_ptr, in_adj, leaf, iters):
    order = np.arange(n).astype(np.int64)
    side = np.zeros(n, np.int8)
    deg1 = np.zeros(n, np.int32)
    deg2 = np.zeros(n, np.int32)
    gain = np.zeros(n, np.float64)
    log2t = np.zeros(n + 3)
    for i in range(1, n + 3):
        log2t[i] = np.log2(i)
    stack_lo = np.empty(4 * 64 + 8, np.int64)
    stack_hi = np.empty(4 * 64 + 8, np.int64)
    stack_lo[0] = 0
    stack_hi[0] = n
    sp = 1
    while sp > 0:
        sp -= 1
        lo = stack_lo[sp]
        hi = stack_hi[sp]
        if hi - lo <= leaf:
            continue
        _bisect(order, lo, hi, in_ptr, in_adj, side, deg1, deg2, gain, log2t, iters)
        mid = (lo + hi) // 2
        stack_lo[sp] = lo
        stack_hi[sp] = mid
        sp += 1
        stack_lo[sp] = mid
        stack_hi[sp] = hi
        sp += 1
    return order


def bisection_order(pre: np.ndarray, post: np.ndarray, n: int, leaf: int = 16, iters: int = 20) -> np.ndarray:
    """Return ``perm`` with ``perm[old_index] = new_index``.

    Deterministic: depends only on the edge list.
    """
    if n == 0:
        return np.zeros(0, np.int64)
    o = np.argsort(post, kind="stable")
    in_adj = pre[o].astype(np.int32)
    in_ptr = np.zeros(n + 1, np.int64)
    np.add.at(in_ptr, post.astype(np.int64) + 1, 1)
    np.cumsum(in_ptr, out=in_ptr)
    order = _bp_order(n, in_ptr, in_adj, leaf, iters)
    perm = np.empty(n, np.int64)
    perm[order] = np.arange(n)
    return perm


def edge_digest(pre: np.ndarray, post: np.ndarray, n: int) -> str:
    """Fingerprint of the edge set the ordering was computed for."""
    h = hashlib.sha256(str(n).encode())
    h.update(np.ascontiguousarray(pre, np.int32).tobytes())
    h.update(np.ascontiguousarray(post, np.int32).tobytes())
    return h.hexdigest()


def cached_order(pre: np.ndarray, post: np.ndarray, n: int) -> np.ndarray:
    config.CACHE.mkdir(parents=True, exist_ok=True)
    path = config.CACHE / "perm_bp.npz"
    digest = edge_digest(pre, post, n)
    if path.exists():
        d = np.load(path)
        if str(d["digest"]) == digest and len(d["perm"]) == n:
            return d["perm"]
    perm = bisection_order(pre, post, n)
    np.savez(path, perm=perm, digest=digest)
    return perm
