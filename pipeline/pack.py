"""Write the calculator data files (meta + one file per weight tier).

Layout mirrors the structs in core/flysim.h; everything is little endian and
4-byte aligned so the C side can use the bytes in place.
"""

from __future__ import annotations

import json
import logging
import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import codec, config
from .model import FRAC, ModelParams

log = logging.getLogger(__name__)

MAGIC = b"FLYCNS01"
VERSION = 5
MAX_TIERS = 2
CHUNK_BYTES = 1 << 18
BLOCK_SLACK = 16
SIGN_EXC, SIGN_INH, SIGN_NONE = 0, 1, 2
SIDE_SHIFT = 2
SIDE_CODE = {"": 0, "L": 1, "R": 2}

META_NAME = "flymeta.tns"
TIER_NAMES = ("flycore.tns", "flyweak.tns")


@dataclass(frozen=True)
class GroupInfo:
    name: str
    rgb: tuple[int, int, int]
    motor: bool


@dataclass(frozen=True)
class ScenarioInfo:
    name: str
    hint: str
    rate_hz: int
    ids: np.ndarray  # uint32, ordered left / right / unsided
    n_left: int
    n_right: int


@dataclass(frozen=True)
class PackInput:
    indptr: np.ndarray  # int64 [n + 1]
    targets: np.ndarray  # int32 [E], ascending within a row
    weights: np.ndarray  # int32 [E] synapse counts >= 1
    flags: np.ndarray  # uint8 [n]: sign code | side code << 2
    pix: np.ndarray  # uint16 [n] map pixel index
    type_ids: np.ndarray  # uint16 [n]
    type_names: tuple[str, ...]
    type_aliases: tuple[str, ...]  # same length as type_names, '' = none
    group_ids: np.ndarray  # uint8 [n]
    groups: tuple[GroupInfo, ...]
    scenarios: tuple[ScenarioInfo, ...]
    params: ModelParams
    map_w: int
    map_h: int
    seed: int = 0x1234ABCD
    selftests: tuple[tuple[int, int, int, int], ...] = ()  # (scenario, steps, spikes, hash)
    # Spikes each neuron fires across typical use; ranks which blocks of
    # edges stay in RAM when the calculator cannot hold them all.
    usage: np.ndarray | None = None  # float [n] or None
    core_min_weight: int = config.CORE_MIN_WEIGHT
    extra: dict = field(default_factory=dict)

    @property
    def n(self) -> int:
        return len(self.indptr) - 1


def _pad4(b: bytes) -> bytes:
    return b + b"\0" * (-len(b) % 4)


def _fixed_str(s: str, size: int) -> bytes:
    raw = s.encode("ascii", "replace")
    if len(raw) >= size:
        raise ValueError(f"string too long for {size - 1} chars: {s!r}")
    return raw.ljust(size, b"\0")


def rgb565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def split_tiers(inp: PackInput):
    """[(indptr, targets, weights)] for the core tier then the weak tier."""
    n = inp.n
    rows = np.repeat(np.arange(n), np.diff(inp.indptr))
    out = []
    core = inp.weights >= inp.core_min_weight
    for mask in (core, ~core):
        counts = np.bincount(rows[mask], minlength=n)
        indptr = np.concatenate([[0], np.cumsum(counts)]).astype(np.int64)
        out.append((indptr, inp.targets[mask].astype(np.int64), inp.weights[mask].astype(np.int64)))
    return out


def check_no_overflow(inp: PackInput) -> int:
    """Largest |g| any neuron can reach must fit in int32 (see flysim.c).

    Worst case: every input fires at the refractory limit, so the per-step
    input is at most the neuron's total afferent synapse count; the decaying
    sum is bounded by input / (1 - decay_g), plus one undecayed helping per
    refractory step (g is frozen then).  Returns the bound (fixed point).
    """
    total_in = np.zeros(inp.n, np.int64)
    np.add.at(total_in, inp.targets, inp.weights)
    per_step = int(total_in.max(initial=0)) * (1 << FRAC)
    bound = int(per_step * (1.0 / (1.0 - inp.params.decay_g) + inp.params.refrac_steps + 1)) + 1
    if bound >= 2**31:
        raise ValueError(f"synaptic variable could overflow int32 (bound {bound})")
    return bound


def hot_unit_order(usage: np.ndarray, block_offs: list[np.ndarray], block_rows: int = config.BLOCK_ROWS) -> np.ndarray:
    """Rank every (tier, block) unit by how much it is worth keeping in RAM:
    expected spikes served per byte.  Returns uint16 codes ``tier << 15 |
    block``, best first.  Units nobody used come last, core tier first."""
    n_blocks = len(block_offs[0]) - 1
    pad = n_blocks * block_rows - len(usage)
    per_block = np.pad(np.asarray(usage, float), (0, pad)).reshape(n_blocks, block_rows).sum(axis=1)
    density, codes = [], []
    for tier, block_off in enumerate(block_offs):
        density.append(per_block / np.maximum(np.diff(block_off.astype(np.int64)), 1))
        codes.append((tier << 15) | np.arange(n_blocks))
    density, codes = np.concatenate(density), np.concatenate(codes)
    return codes[np.lexsort((codes, -density))].astype(np.uint16)


def _tier_section(file_name: str, code: codec.TierCode, data: bytes, block_off: np.ndarray) -> bytes:
    sizes = np.diff(block_off.astype(np.int64))
    max_block = int(sizes.max(initial=0))
    if max_block > CHUNK_BYTES - BLOCK_SLACK - 4:
        raise ValueError(f"block of {max_block} bytes does not fit a {CHUNK_BYTES}-byte chunk")
    head = _fixed_str(file_name, 32) + struct.pack("<4I", code.wmin, int(code.has_weights), len(data), max_block)
    return (
        head
        + code.gap_lut.astype("<u2").tobytes()
        + code.w_lut.astype("<u2").tobytes()
        + block_off.astype("<u4").tobytes()
    )


def _scenario_section(scenarios: tuple[ScenarioInfo, ...]) -> bytes:
    rec = 20 + 28 + 5 * 4
    pool_off = len(scenarios) * rec
    recs, pool = [], []
    for sc in scenarios:
        recs.append(
            _fixed_str(sc.name, 20)
            + _fixed_str(sc.hint, 28)
            + struct.pack("<5I", sc.rate_hz, len(sc.ids), sc.n_left, sc.n_right, pool_off)
        )
        blob = np.asarray(sc.ids, "<u4").tobytes()
        pool.append(blob)
        pool_off += len(blob)
    return b"".join(recs) + b"".join(pool)


def _type_names_section(names: tuple[str, ...]) -> bytes:
    blobs = [n.encode("ascii", "replace") for n in names]
    offs = np.concatenate([[0], np.cumsum([len(b) for b in blobs])]).astype("<u4")
    return offs.tobytes() + b"".join(blobs)


def write_pack(inp: PackInput, out_dir: Path) -> dict:
    """Encode and write all files; returns a manifest (also saved as JSON)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    n = inp.n
    if not (len(inp.flags) == len(inp.pix) == len(inp.type_ids) == len(inp.group_ids) == n):
        raise ValueError("per-neuron arrays must all have length n")
    if n == 0 or n >= 1 << 18:
        raise ValueError("neuron count must be in 1..262143")
    if len(inp.type_names) > 0xFFFF or len(inp.groups) > 0xFF:
        raise ValueError("too many types or groups")
    if len(inp.type_aliases) != len(inp.type_names):
        raise ValueError("type_aliases must parallel type_names")
    if inp.type_ids.max(initial=0) >= len(inp.type_names) or inp.group_ids.max(initial=0) >= len(inp.groups):
        raise ValueError("type or group id out of range")
    if inp.pix.max(initial=0) >= inp.map_w * inp.map_h:
        raise ValueError("pixel index outside the map")
    if len(inp.weights) and inp.weights.min() < 1:
        raise ValueError("weights must be >= 1")
    if len(inp.targets) and (inp.targets.min() < 0 or inp.targets.max() >= n):
        raise ValueError("edge target outside 0..n-1")
    for sc in inp.scenarios:
        if len(sc.ids) and int(np.max(sc.ids)) >= n:
            raise ValueError(f"scenario {sc.name!r} names a neuron outside 0..n-1")
    if inp.seed == 0:
        raise ValueError("seed must be non-zero (xorshift)")
    g_bound = check_no_overflow(inp)
    n_blocks = (n + config.BLOCK_ROWS - 1) // config.BLOCK_ROWS
    fx = inp.params.fixed()

    tiers = split_tiers(inp)
    if inp.usage is not None and len(inp.usage) != n:
        raise ValueError("usage must have one entry per neuron")
    tier_manifest, tier_sections, block_offs = [], [], []
    n_edges, n_syn = [0, 0], [0, 0]
    for k, (indptr, targets, weights) in enumerate(tiers):
        code = codec.fit_code(indptr, targets, weights)
        data, block_off = codec.encode_tier(indptr, targets, weights, code)
        (out_dir / TIER_NAMES[k]).write_bytes(data)
        tier_sections.append(_tier_section(TIER_NAMES[k], code, data, block_off))
        block_offs.append(block_off)
        n_edges[k], n_syn[k] = len(targets), int(weights.sum())
        bits = 8 * len(data) / max(1, len(targets))
        log.info(
            "tier %d: %s edges, %s synapses, %.2f MB, %.2f bits/edge",
            k,
            f"{len(targets):,}",
            f"{n_syn[k]:,}",
            len(data) / 1e6,
            bits,
        )
        tier_manifest.append(
            {
                "file": TIER_NAMES[k],
                "edges": len(targets),
                "synapses": n_syn[k],
                "bytes": len(data),
                "bits_per_edge": bits,
                "wmin": code.wmin,
                "has_weights": code.has_weights,
                "max_block_bytes": int(np.diff(block_off.astype(np.int64)).max(initial=0)),
            }
        )

    type_counts = np.bincount(inp.type_ids, minlength=len(inp.type_names)).astype(np.int64)
    if type_counts.max(initial=0) > 0xFFFF:
        raise ValueError("a type has more than 65535 neurons")
    group_sizes = np.bincount(inp.group_ids, minlength=len(inp.groups))
    groups_blob = b"".join(
        _fixed_str(g.name, 12) + struct.pack("<HHI", rgb565(g.rgb), int(g.motor), int(group_sizes[i]))
        for i, g in enumerate(inp.groups)
    )
    sections = [
        (b"NFLG", inp.flags.astype("u1").tobytes()),
        (b"NPIX", inp.pix.astype("<u2").tobytes()),
        (b"NTYP", inp.type_ids.astype("<u2").tobytes()),
        (b"NGRP", inp.group_ids.astype("u1").tobytes()),
        (b"TNAM", _type_names_section(inp.type_names)),
        (b"TALI", _type_names_section(inp.type_aliases)),
        (b"TCNT", type_counts.astype("<u2").tobytes()),
        (b"GRPS", groups_blob),
        (b"SCEN", _scenario_section(inp.scenarios)),
        (b"TEST", b"".join(struct.pack("<4I", *t) for t in inp.selftests)),
        (b"TIR0", tier_sections[0]),
        (b"TIR1", tier_sections[1]),
    ]
    if inp.usage is not None:
        sections.append((b"HOTS", hot_unit_order(inp.usage, block_offs).astype("<u2").tobytes()))
    header_size = 8 + 4 * 24
    dir_size = 12 * len(sections)
    offset = header_size + dir_size
    directory, body = [], []
    for tag, blob in sections:
        blob = _pad4(blob)
        directory.append(tag + struct.pack("<II", offset, len(blob)))
        body.append(blob)
        offset += len(blob)
    header = MAGIC + struct.pack(
        "<7I2I2I3i3I2III2II",
        VERSION,
        n,
        len(inp.type_names),
        len(inp.groups),
        len(inp.scenarios),
        n_blocks,
        MAX_TIERS,
        *n_edges,
        *n_syn,
        fx.dv,
        fx.dg,
        fx.cv,
        inp.params.v_thresh_uv,
        0,
        inp.params.w_syn_uv,
        fx.delay,
        fx.refrac,
        fx.dt_us,
        inp.seed,
        inp.map_w,
        inp.map_h,
        len(sections),
    )
    if len(header) != header_size:
        raise AssertionError("header layout drifted from fs_header_t")
    meta = header + b"".join(directory) + b"".join(body)
    (out_dir / META_NAME).write_bytes(meta)

    manifest = {
        "version": VERSION,
        "neurons": n,
        "types": len(inp.type_names),
        "blocks": n_blocks,
        "edges_total": int(sum(n_edges)),
        "synapses_total": int(sum(n_syn)),
        "meta": {"file": META_NAME, "bytes": len(meta)},
        "tiers": tier_manifest,
        "model": inp.params.describe(),
        "g_bound_fixed": g_bound,
        "seed": inp.seed,
        "selftests": [dict(zip(("scenario", "steps", "spikes", "hash"), t)) for t in inp.selftests],
        **inp.extra,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return manifest
