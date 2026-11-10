"""Shared fixtures: a small synthetic connectome and the host binaries."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import numpy as np
import pytest

from pipeline import pack
from pipeline.model import ModelParams

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "host" / "flysim_cli"
CLI_ASAN = ROOT / "build" / "host" / "flysim_cli_asan"
PREVIEW_ASAN = ROOT / "build" / "host" / "ui_preview_asan"
REAL_DATA = ROOT / "build" / "calc"
N_GROUPS = 27  # the UI indexes groups 0..26 by constant (ui/app.h)


def synthetic_graph(n: int = 1500, seed: int = 3, hub_rows: int = 3):
    """Random directed graph with clustered targets, heavy-tailed weights,
    empty rows and a few very wide rows - the shapes the codec must survive."""
    rng = np.random.default_rng(seed)
    rows, cols, weights = [], [], []
    for i in range(n):
        if rng.random() < 0.08:
            continue  # neuron with no outputs
        deg = n - 1 if i < hub_rows else int(rng.integers(1, 60))
        centre = int(rng.integers(0, n))
        near = (centre + rng.integers(-40, 40, size=deg)) % n
        far = rng.integers(0, n, size=deg)
        tgt = np.unique(np.where(rng.random(deg) < 0.7, near, far))
        if i < hub_rows:
            tgt = np.setdiff1d(np.arange(n), [i])
        w = np.maximum(1, rng.geometric(0.45, size=len(tgt)))
        big = rng.random(len(tgt)) < 0.02
        w = np.where(big, rng.integers(50, 2600, size=len(tgt)), w)
        rows.append(np.full(len(tgt), i))
        cols.append(tgt)
        weights.append(w)
    rows = np.concatenate(rows)
    cols = np.concatenate(cols).astype(np.int32)
    weights = np.concatenate(weights).astype(np.int32)
    indptr = np.zeros(n + 1, np.int64)
    np.add.at(indptr, rows + 1, 1)
    return np.cumsum(indptr), cols, weights


def synthetic_input(n: int = 1500, seed: int = 3, params: ModelParams | None = None) -> pack.PackInput:
    indptr, targets, weights = synthetic_graph(n, seed)
    rng = np.random.default_rng(seed + 100)
    sign = rng.choice([pack.SIGN_EXC, pack.SIGN_INH, pack.SIGN_NONE], size=n, p=[0.6, 0.35, 0.05]).astype(np.uint8)
    side = rng.integers(0, 3, size=n).astype(np.uint8)
    n_types = 40
    type_ids = rng.integers(0, n_types, size=n).astype(np.uint16)
    type_ids[:n_types] = np.arange(n_types)  # every type occurs
    groups = tuple(pack.GroupInfo(f"g{i}", (9 * i, 255 - 9 * i, 128), i % 2 == 1) for i in range(N_GROUPS))
    stim = np.sort(rng.choice(n, size=60, replace=False)).astype(np.uint32)
    scenarios = (
        pack.ScenarioInfo("alpha", "first", 120, stim, 20, 25),
        pack.ScenarioInfo("beta", "second", 60, stim[:10], 4, 3),
    )
    return pack.PackInput(
        indptr=indptr,
        targets=targets,
        weights=weights,
        flags=sign | (side << pack.SIDE_SHIFT),
        pix=rng.integers(0, 180 * 240, size=n).astype(np.uint16),
        type_ids=type_ids,
        type_names=tuple(["(untyped)"] + [f"Type{i:02d}" for i in range(1, n_types)]),
        type_aliases=tuple([""] + [f"al{i}" if i % 3 == 0 else "" for i in range(1, n_types)]),
        group_ids=rng.integers(0, N_GROUPS, size=n).astype(np.uint8),
        groups=groups,
        scenarios=scenarios,
        # Strong coupling so a 1500-neuron random net actually propagates.
        params=params or ModelParams(w_syn_mv=0.9),
        map_w=180,
        map_h=240,
        selftests=((0, 50, 123, 456), (1, 20, 7, 8)),  # placeholders: layout only
    )


def sign_of(flags: np.ndarray) -> np.ndarray:
    code = flags & 3
    return np.select([code == pack.SIGN_INH, code == pack.SIGN_NONE], [-1, 0], 1).astype(np.int8)


def run_cli(binary: Path, data_dir: Path, *args: str) -> dict:
    out = subprocess.run([str(binary), str(data_dir), *args], check=False, capture_output=True, text=True, timeout=600)
    assert out.returncode == 0, f"{binary.name} failed: {out.stderr}"
    assert "ERROR: AddressSanitizer" not in out.stderr and "runtime error" not in out.stderr, out.stderr
    return json.loads(out.stdout)


@pytest.fixture(scope="session")
def host_binaries():
    for b in (CLI, CLI_ASAN, PREVIEW_ASAN):
        if not b.exists():
            pytest.skip(f"{b} not built (run `make host`)")
    return {"cli": CLI, "asan": CLI_ASAN, "preview": PREVIEW_ASAN}


@pytest.fixture(scope="session")
def synth(tmp_path_factory):
    inp = synthetic_input()
    out = tmp_path_factory.mktemp("synth")
    manifest = pack.write_pack(inp, out)
    return inp, out, manifest


@pytest.fixture(scope="session")
def real_data():
    if not (REAL_DATA / "flymeta.tns").exists():
        pytest.skip("real data not built (run `make data`)")
    return REAL_DATA
