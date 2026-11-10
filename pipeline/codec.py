"""Entropy codec for connectome rows; the decoder lives in core/flysim.c.

A row is one presynaptic neuron's outgoing connections sorted by target.  Each
edge is a target gap (target - previous target, starting from -1) and, in the
core tier, a weight offset (synapses - wmin).  Both are bucketed: small values
get their own Huffman symbol, large ones a log2 bucket symbol followed by raw
mantissa bits.  Codes are length-limited so the calculator can decode a symbol
with a single small table lookup.

Rows are grouped into blocks of BLOCK_ROWS neurons:

    uint16 row_bytes[BLOCK_ROWS]            (0 = neuron has no edges here)
    per row:  LEB128 edge count, bit stream (MSB first), padded to a byte
"""

from __future__ import annotations

import heapq
from dataclasses import dataclass

import numba as nb
import numpy as np

from .config import BLOCK_ROWS

GAP_LUT_BITS = 10
W_LUT_BITS = 9
GAP_EXACT = 16
W_EXACT = 8
N_GAP_SYMS = 30  # exact 1..16, then log2 buckets up to 2**18
N_W_SYMS = 20  # exact 0..7, then log2 buckets up to 2**15
MAX_GAP = (1 << 18) - 1
MAX_WEIGHT_OFFSET = (1 << 15) - 1


@nb.njit(cache=True, inline="always")
def _ilog2(x):
    k = 0
    while x > 1:
        x >>= 1
        k += 1
    return k


@nb.njit(cache=True, inline="always")
def gap_symbol(gap):
    """gap >= 1 -> (symbol, extra_bits, n_extra)."""
    if gap <= GAP_EXACT:
        return gap - 1, 0, 0
    x = gap - 1
    k = _ilog2(x)
    return GAP_EXACT + k - 4, x - (1 << k), k


@nb.njit(cache=True, inline="always")
def weight_symbol(y):
    """y = weight - wmin >= 0 -> (symbol, extra_bits, n_extra)."""
    if y < W_EXACT:
        return y, 0, 0
    k = _ilog2(y)
    return W_EXACT + k - 3, y - (1 << k), k


@nb.njit(cache=True)
def symbol_histograms(indptr, targets, weights, wmin):
    gh = np.zeros(N_GAP_SYMS, np.int64)
    wh = np.zeros(N_W_SYMS, np.int64)
    n = len(indptr) - 1
    for r in range(n):
        prev = -1
        for j in range(indptr[r], indptr[r + 1]):
            s, _, _ = gap_symbol(targets[j] - prev)
            prev = targets[j]
            gh[s] += 1
            s, _, _ = weight_symbol(weights[j] - wmin)
            wh[s] += 1
    return gh, wh


def limited_code_lengths(freq: np.ndarray, max_len: int) -> np.ndarray:
    """Huffman code lengths with a hard cap (heuristic Kraft repair).

    Symbols with zero frequency get length 0 (no code).
    """
    freq = np.asarray(freq, np.int64)
    used = np.flatnonzero(freq > 0)
    lengths = np.zeros(len(freq), np.int64)
    if len(used) == 0:
        return lengths
    if len(used) == 1:
        lengths[used[0]] = 1
        return lengths
    if len(used) > (1 << max_len):
        raise ValueError("alphabet too large for the length limit")
    heap = [(int(freq[s]), int(s), (int(s),)) for s in used]
    heapq.heapify(heap)
    depth = {int(s): 0 for s in used}
    while len(heap) > 1:
        fa, ta, sa = heapq.heappop(heap)
        fb, tb, sb = heapq.heappop(heap)
        for s in sa + sb:
            depth[s] += 1
        heapq.heappush(heap, (fa + fb, min(ta, tb), sa + sb))
    for s, d in depth.items():
        lengths[s] = min(d, max_len)
    # Clamping may over-subscribe the code space; lengthen the cheapest
    # (least frequent) symbols that are still shorter than the cap.
    unit = 1 << max_len

    def kraft() -> int:
        return int(sum(unit >> int(lengths[s]) for s in used))

    order = sorted(used, key=lambda s: (freq[s], s))
    while kraft() > unit:
        for s in order:
            if lengths[s] < max_len:
                lengths[s] += 1
                break
        else:  # pragma: no cover - guarded by the alphabet-size check
            raise RuntimeError("cannot satisfy Kraft inequality")
    # Then hand unused space back to the most frequent symbols.
    for s in reversed(order):
        while lengths[s] > 1 and kraft() + (unit >> int(lengths[s])) <= unit:
            lengths[s] -= 1
    return lengths


def canonical_codes(lengths: np.ndarray) -> np.ndarray:
    """Canonical Huffman codes (MSB-first) for the given lengths."""
    codes = np.zeros(len(lengths), np.int64)
    code = 0
    prev_len = 0
    for length, sym in sorted((int(l), s) for s, l in enumerate(lengths) if l > 0):
        code <<= length - prev_len
        codes[sym] = code
        code += 1
        prev_len = length
    return codes


def build_lut(lengths: np.ndarray, codes: np.ndarray, bits: int) -> np.ndarray:
    """Direct lookup: top `bits` bits of the stream -> (length << 8) | symbol."""
    lut = np.zeros(1 << bits, np.uint16)
    for sym, length in enumerate(lengths):
        length = int(length)
        if length == 0:
            continue
        if length > bits:
            raise ValueError("code longer than lookup width")
        lo = int(codes[sym]) << (bits - length)
        lut[lo : lo + (1 << (bits - length))] = (length << 8) | sym
    return lut


@dataclass(frozen=True)
class TierCode:
    wmin: int
    has_weights: bool
    gap_len: np.ndarray
    gap_code: np.ndarray
    w_len: np.ndarray
    w_code: np.ndarray

    @property
    def gap_lut(self) -> np.ndarray:
        return build_lut(self.gap_len, self.gap_code, GAP_LUT_BITS)

    @property
    def w_lut(self) -> np.ndarray:
        if not self.has_weights:
            return np.zeros(1 << W_LUT_BITS, np.uint16)
        return build_lut(self.w_len, self.w_code, W_LUT_BITS)


def fit_code(indptr, targets, weights) -> TierCode:
    """Choose wmin and Huffman codes for one tier."""
    if len(weights) == 0:
        zeros = np.zeros(N_GAP_SYMS, np.int64)
        return TierCode(1, False, zeros, zeros, np.zeros(N_W_SYMS, np.int64), np.zeros(N_W_SYMS, np.int64))
    wmin = int(weights.min())
    if int(weights.max()) - wmin > MAX_WEIGHT_OFFSET:
        raise ValueError(f"weight range {wmin}..{int(weights.max())} exceeds the codec limit")
    if len(indptr) - 1 > MAX_GAP or int(targets.max()) >= MAX_GAP or int(targets.min()) < 0:
        raise ValueError("target index outside the codec range")
    rows = np.repeat(np.arange(len(indptr) - 1), np.diff(indptr))
    if (np.diff(targets)[rows[1:] == rows[:-1]] <= 0).any():
        raise ValueError("targets must be strictly ascending within a row (sorted, no duplicates)")
    has_weights = bool(weights.max() != wmin)
    gh, wh = symbol_histograms(indptr, targets, weights, wmin)
    gl = limited_code_lengths(gh, GAP_LUT_BITS)
    wl = limited_code_lengths(wh, W_LUT_BITS) if has_weights else np.zeros(N_W_SYMS, np.int64)
    return TierCode(wmin, has_weights, gl, canonical_codes(gl), wl, canonical_codes(wl))


@nb.njit(cache=True)
def _encode(indptr, targets, weights, wmin, has_weights, gap_len, gap_code, w_len, w_code, block_rows, out, block_off):
    n = len(indptr) - 1
    n_blocks = (n + block_rows - 1) // block_rows
    pos = 0
    for b in range(n_blocks):
        block_off[b] = pos
        header = pos
        pos += 2 * block_rows
        for k in range(2 * block_rows):
            out[header + k] = 0
        for r in range(b * block_rows, min(n, (b + 1) * block_rows)):
            a, e = indptr[r], indptr[r + 1]
            if e == a:
                continue
            start = pos
            c = e - a
            while True:
                byte = c & 0x7F
                c >>= 7
                if c:
                    out[pos] = byte | 0x80
                    pos += 1
                else:
                    out[pos] = byte
                    pos += 1
                    break
            acc = np.uint64(0)
            nbits = 0
            prev = -1
            for j in range(a, e):
                sym, extra, nx = gap_symbol(targets[j] - prev)
                prev = targets[j]
                acc = (acc << np.uint64(gap_len[sym])) | np.uint64(gap_code[sym])
                nbits += gap_len[sym]
                if nx:
                    acc = (acc << np.uint64(nx)) | np.uint64(extra)
                    nbits += nx
                while nbits >= 8:
                    nbits -= 8
                    out[pos] = (acc >> np.uint64(nbits)) & np.uint64(0xFF)
                    pos += 1
                if has_weights:
                    sym, extra, nx = weight_symbol(weights[j] - wmin)
                    acc = (acc << np.uint64(w_len[sym])) | np.uint64(w_code[sym])
                    nbits += w_len[sym]
                    if nx:
                        acc = (acc << np.uint64(nx)) | np.uint64(extra)
                        nbits += nx
                    while nbits >= 8:
                        nbits -= 8
                        out[pos] = (acc >> np.uint64(nbits)) & np.uint64(0xFF)
                        pos += 1
            if nbits:
                out[pos] = (acc << np.uint64(8 - nbits)) & np.uint64(0xFF)
                pos += 1
            size = pos - start
            if size > 0xFFFF:
                return -1 - r
            k = r - b * block_rows
            out[header + 2 * k] = size & 0xFF
            out[header + 2 * k + 1] = size >> 8
    block_off[n_blocks] = pos
    return pos


def encode_tier(indptr, targets, weights, code: TierCode, block_rows: int = BLOCK_ROWS):
    """Return (bytes, block_off[n_blocks + 1]) for one tier."""
    n = len(indptr) - 1
    n_blocks = (n + block_rows - 1) // block_rows
    # Worst case per edge: 10 + 17 + 9 + 14 = 50 bits < 7 bytes; per row a
    # 4-byte count and one byte of padding; plus the block headers.
    cap = 7 * len(targets) + n_blocks * 2 * block_rows + 6 * n + 64
    out = np.zeros(cap, np.uint8)
    block_off = np.zeros(n_blocks + 1, np.uint32)
    size = _encode(
        indptr,
        targets,
        weights,
        code.wmin,
        code.has_weights,
        code.gap_len,
        code.gap_code,
        code.w_len,
        code.w_code,
        block_rows,
        out,
        block_off,
    )
    if size < 0:
        raise ValueError(f"row {-1 - size} exceeds 65535 encoded bytes")
    return out[:size].tobytes(), block_off


@nb.njit(cache=True)
def _decode_row(data, block_off, block_rows, r, gap_lut, w_lut, wmin, has_weights, targets, weights):
    """Python mirror of the C row decoder; returns the edge count."""
    blk = block_off[r // block_rows]
    k = r % block_rows
    size = int(data[blk + 2 * k]) | (int(data[blk + 2 * k + 1]) << 8)
    if size == 0:
        return 0
    p = blk + 2 * block_rows
    for q in range(k):
        p += int(data[blk + 2 * q]) | (int(data[blk + 2 * q + 1]) << 8)
    count = 0
    shift = 0
    while True:
        byte = int(data[p])
        p += 1
        count |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            break
    buf = np.uint64(0)
    cnt = 0
    t = -1
    limit = len(data)
    for e in range(count):
        while cnt <= 24:
            byte = int(data[p]) if p < limit else 0
            p += 1
            buf |= np.uint64(byte) << np.uint64(24 - cnt)
            cnt += 8
        entry = int(gap_lut[int(buf >> np.uint64(32 - GAP_LUT_BITS))])
        sym = entry & 0xFF
        ln = entry >> 8
        buf = (buf << np.uint64(ln)) & np.uint64(0xFFFFFFFF)
        cnt -= ln
        if sym < GAP_EXACT:
            t += sym + 1
        else:
            kk = sym - GAP_EXACT + 4
            while cnt <= 24:
                byte = int(data[p]) if p < limit else 0
                p += 1
                buf |= np.uint64(byte) << np.uint64(24 - cnt)
                cnt += 8
            t += ((1 << kk) | int(buf >> np.uint64(32 - kk))) + 1
            buf = (buf << np.uint64(kk)) & np.uint64(0xFFFFFFFF)
            cnt -= kk
        w = 0
        if has_weights:
            while cnt <= 24:
                byte = int(data[p]) if p < limit else 0
                p += 1
                buf |= np.uint64(byte) << np.uint64(24 - cnt)
                cnt += 8
            entry = int(w_lut[int(buf >> np.uint64(32 - W_LUT_BITS))])
            sym = entry & 0xFF
            ln = entry >> 8
            buf = (buf << np.uint64(ln)) & np.uint64(0xFFFFFFFF)
            cnt -= ln
            if sym < W_EXACT:
                w = sym
            else:
                kk = sym - W_EXACT + 3
                w = (1 << kk) | int(buf >> np.uint64(32 - kk))
                buf = (buf << np.uint64(kk)) & np.uint64(0xFFFFFFFF)
                cnt -= kk
        targets[e] = t
        weights[e] = w + wmin
    return count


def decode_row(data: bytes, block_off, r: int, code: TierCode, block_rows: int = BLOCK_ROWS, max_edges: int = 1 << 18):
    buf = np.frombuffer(data, np.uint8)
    t = np.zeros(max_edges, np.int64)
    w = np.zeros(max_edges, np.int64)
    c = _decode_row(
        buf, np.asarray(block_off, np.int64), block_rows, r, code.gap_lut, code.w_lut, code.wmin, code.has_weights, t, w
    )
    return t[:c], w[:c]
