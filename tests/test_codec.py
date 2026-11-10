from __future__ import annotations

import itertools

import numpy as np
import pytest

from pipeline import codec

from .conftest import synthetic_graph

pytestmark = pytest.mark.unit


@pytest.mark.parametrize("gap", [1, 2, 15, 16, 17, 18, 31, 32, 33, 1000, 65536, 166699, (1 << 18) - 1])
def test_gap_symbol_roundtrip(gap):
    sym, extra, nbits = codec.gap_symbol(gap)
    assert 0 <= sym < codec.N_GAP_SYMS
    if sym < codec.GAP_EXACT:
        assert (nbits, sym + 1) == (0, gap)
    else:
        k = sym - codec.GAP_EXACT + 4
        assert nbits == k and 0 <= extra < (1 << k)
        assert ((1 << k) | extra) + 1 == gap


@pytest.mark.parametrize("y", [0, 1, 7, 8, 9, 15, 16, 100, 2589, codec.MAX_WEIGHT_OFFSET])
def test_weight_symbol_roundtrip(y):
    sym, extra, nbits = codec.weight_symbol(y)
    assert 0 <= sym < codec.N_W_SYMS
    if sym < codec.W_EXACT:
        assert (nbits, sym) == (0, y)
    else:
        k = sym - codec.W_EXACT + 3
        assert nbits == k and ((1 << k) | extra) == y


@pytest.mark.parametrize(
    "freq",
    [
        [5, 0, 0, 0],
        [1, 1],
        [1000, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
        list(2 ** np.arange(24)),  # forces the length cap
        [3, 3, 3, 3, 3, 3, 3, 3],
    ],
)
def test_limited_code_lengths_are_prefix_free(freq):
    max_len = 6
    lengths = codec.limited_code_lengths(np.array(freq), max_len)
    used = [int(l) for l, f in zip(lengths, freq) if f > 0]
    assert all(1 <= l <= max_len for l in used)
    assert all(int(l) == 0 for l, f in zip(lengths, freq) if f == 0)
    assert sum(2.0**-l for l in used) <= 1.0 + 1e-12  # Kraft
    codes = codec.canonical_codes(lengths)
    words = sorted(format(int(c), f"0{int(l)}b") for c, l in zip(codes, lengths) if l > 0)
    assert all(not b.startswith(a) for a, b in itertools.pairwise(words))


def test_limited_code_lengths_rejects_oversized_alphabet():
    with pytest.raises(ValueError):
        codec.limited_code_lengths(np.ones(9), 3)


def test_lut_maps_every_prefix_to_its_symbol_and_length():
    bits = 5
    lengths = codec.limited_code_lengths(np.array([40, 30, 20, 5, 3, 1, 1]), bits)
    codes = codec.canonical_codes(lengths)
    lut = codec.build_lut(lengths, codes, bits)
    covered = 0
    for sym, length in enumerate(int(l) for l in lengths):
        lo = int(codes[sym]) << (bits - length)
        span = lut[lo : lo + (1 << (bits - length))]
        assert (span == ((length << 8) | sym)).all(), f"symbol {sym}"
        covered += len(span)
    # A complete code leaves no unused slot (unused slots would read as 0).
    assert covered == 1 << bits
    with pytest.raises(ValueError):
        codec.build_lut(np.array([7, 7]), np.array([0, 1]), bits)


@pytest.mark.parametrize("seed", [0, 1, 2])
def test_tier_roundtrip_all_rows(seed):
    indptr, targets, weights = synthetic_graph(700, seed)
    for tier_weights in (weights, np.ones_like(weights)):  # weighted and weightless tiers
        code = codec.fit_code(indptr, targets, tier_weights)
        data, block_off = codec.encode_tier(indptr, targets, tier_weights, code)
        assert block_off[-1] == len(data)
        for r in range(len(indptr) - 1):
            t, w = codec.decode_row(data, block_off, r, code)
            a, b = indptr[r], indptr[r + 1]
            assert np.array_equal(t, targets[a:b]), f"row {r} targets"
            assert np.array_equal(w, tier_weights[a:b]), f"row {r} weights"


def test_empty_tier_encodes():
    indptr = np.zeros(11, np.int64)
    empty = np.zeros(0, np.int64)
    code = codec.fit_code(indptr, empty, empty)
    data, block_off = codec.encode_tier(indptr, empty, empty, code)
    assert len(data) == 2 * codec.BLOCK_ROWS and block_off.tolist() == [0, len(data)]
    t, w = codec.decode_row(data, block_off, 3, code)
    assert len(t) == 0 and len(w) == 0


def test_weightless_tier_stores_no_weight_bits():
    indptr, targets, weights = synthetic_graph(400, 5)
    ones = np.ones_like(weights)
    with_w = codec.encode_tier(indptr, targets, weights, codec.fit_code(indptr, targets, weights))[0]
    without = codec.encode_tier(indptr, targets, ones, codec.fit_code(indptr, targets, ones))[0]
    assert len(without) < len(with_w)
    assert not codec.fit_code(indptr, targets, ones).has_weights


def test_out_of_range_values_are_rejected():
    indptr = np.array([0, 2], np.int64)
    with pytest.raises(ValueError, match="weight range"):
        codec.fit_code(indptr, np.array([0, 1]), np.array([1, codec.MAX_WEIGHT_OFFSET + 2]))
    with pytest.raises(ValueError, match="target index"):
        codec.fit_code(indptr, np.array([0, codec.MAX_GAP]), np.array([1, 2]))


@pytest.mark.parametrize("targets", [[5, 5, 7], [7, 5, 9], [3, 2, 1]])
def test_unsorted_or_duplicate_targets_are_rejected(targets):
    with pytest.raises(ValueError, match="strictly ascending"):
        codec.fit_code(np.array([0, 3], np.int64), np.array(targets), np.array([2, 3, 4]))


def test_extreme_symbols_roundtrip_through_the_bitstream():
    """Largest gap bucket and largest weight bucket in one row."""
    n = codec.MAX_GAP
    indptr = np.zeros(n + 1, np.int64)
    indptr[1:] = 4
    targets = np.array([0, 1, n - 2, n - 1], np.int64)
    weights = np.array([1, 1 + codec.MAX_WEIGHT_OFFSET, 2, 9], np.int64)
    code = codec.fit_code(indptr, targets, weights)
    data, block_off = codec.encode_tier(indptr, targets, weights, code)
    t, w = codec.decode_row(data, block_off, 0, code)
    assert t.tolist() == targets.tolist() and w.tolist() == weights.tolist()
