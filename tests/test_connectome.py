"""End-to-end checks on the real male CNS data (skipped if it is absent)."""

from __future__ import annotations

import json

import numpy as np
import pytest

from pipeline import build, pack
from pipeline.graph import load_ordered
from pipeline.intsim import IntSim

from .conftest import REAL_DATA, run_cli
from .metafile import read_meta

pytestmark = pytest.mark.connectome


@pytest.fixture(scope="module")
def graph(real_data):
    return load_ordered()


@pytest.fixture(scope="module")
def manifest(real_data):
    return json.loads((REAL_DATA / "manifest.json").read_text())


def test_dataset_is_the_full_connectome(graph, manifest):
    assert graph.n == 166_700
    assert graph.n_edges == manifest["edges_total"] == 25_582_938
    assert int(graph.weights.sum()) == manifest["synapses_total"] == 124_177_617
    assert graph.weights.min() == 1  # no synapse-count threshold
    # CSR rows sorted and duplicate-free
    rows = np.repeat(np.arange(graph.n), np.diff(graph.indptr))
    same_row = rows[1:] == rows[:-1]
    assert (np.diff(graph.targets)[same_row] > 0).all()


def test_files_fit_the_calculator(manifest):
    total = manifest["meta"]["bytes"] + sum(t["bytes"] for t in manifest["tiers"])
    assert total < 90 << 20, "must fit the handheld's ~96 MB of free flash"
    assert all(t["max_block_bytes"] + 16 < 1 << 20 for t in manifest["tiers"])
    assert manifest["g_bound_fixed"] < 2**31


def test_calculator_decodes_the_exact_graph(graph, host_binaries, real_data):
    core, weak = graph.tier(True), graph.tier(False)
    rng = np.random.default_rng(1)
    outdeg = np.diff(graph.indptr)
    rows = np.unique(np.concatenate([rng.integers(0, graph.n, 40), [0, graph.n - 1, int(outdeg.argmax())]]))
    for k, (indptr, targets, weights) in enumerate((core, weak)):
        for r in rows:
            got = run_cli(host_binaries["cli"], real_data, "--row", str(k), str(r))["edges"]
            a, b = indptr[r], indptr[r + 1]
            assert [e[0] for e in got] == targets[a:b].tolist()
            assert [e[1] for e in got] == weights[a:b].tolist()


@pytest.mark.parametrize("scenario,steps,arena", [(0, 400, None), (2, 300, None), (1, 200, "24"), (8, 150, "8")])
def test_core_matches_mirror_on_the_connectome(graph, host_binaries, real_data, scenario, steps, arena):
    inp = build.from_connectome(graph)
    sc = inp.scenarios[scenario]
    ref = IntSim(
        graph.indptr, graph.targets, graph.weights, build.effective_sign(inp.flags), inp.params.fixed(), seed=inp.seed
    )
    ref.set_stimuli([(sc.ids, sc.rate_hz)])
    ref.run(steps)
    args = ["--scenario", str(scenario), "--steps", str(steps)] + (["--arena-mb", arena] if arena else [])
    got = run_cli(host_binaries["cli"], real_data, *args)
    assert ref.total_spikes > 500
    assert got["io_errors"] == 0
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)
    if arena:
        assert got["resident_bytes"] < got["total_bytes"]
    if arena == "8":
        assert got["page_faults"] > 100 and got["pool_flushes"] > 0, "paging was not exercised"


def test_builtin_scenarios_run_from_ram_on_a_calculator_sized_arena(host_binaries, real_data):
    """Blocks are pinned by measured usage, so with the ~24 MB a handheld can
    spare the built-in experiments should hardly touch flash."""
    for scenario in (0, 1, 2, 6, 8):
        got = run_cli(
            host_binaries["cli"], real_data, "--scenario", str(scenario), "--steps", "300", "--arena-mb", "24"
        )
        assert got["resident_bytes"] < got["total_bytes"] and got["io_errors"] == 0
        assert got["page_faults"] <= 20, f"scenario {scenario} pages too much: {got['page_faults']}"


def test_selftest_expectations_are_current(graph, manifest, real_data):
    """The hashes the calculator checks at boot must be the ones today's
    model produces, in the manifest and in the meta file itself."""
    inp = build.from_connectome(graph)
    fresh = [tuple(int(x) for x in t) for t in inp.selftests]
    assert [tuple(t.values()) for t in manifest["selftests"]] == fresh
    meta = read_meta(real_data / pack.META_NAME)
    assert meta.selftests() == fresh
    assert meta.header["w_syn_uv"] == build.DEFAULT_PARAMS.w_syn_uv


def test_ui_limits_hold_for_the_real_data(real_data):
    meta = read_meta(real_data / pack.META_NAME)
    counts = np.frombuffer(meta.section("TCNT"), "<u2", meta.header["n_types"])
    assert counts.max() <= 4096, "a cell type no longer fits APP_CUSTOM_CAP"
    assert all(len(s["ids"]) > 0 for s in meta.scenarios())
    assert sum(1 for a in meta.names("TALI") if a) > 100, "literature aliases missing"


def test_layout_and_groups_cover_every_neuron(graph):
    inp = build.from_connectome(graph)
    assert inp.pix.max() < inp.map_w * inp.map_h
    lit = len(np.unique(inp.pix))
    assert lit > 12_000, "map should spread neurons over many pixels"
    assert (inp.group_ids > 0).all(), "every neuron should fall in a named group"
    sizes = np.bincount(inp.group_ids, minlength=len(inp.groups))
    assert (sizes[1:] > 0).all(), "no empty readout group"
    assert sizes[1] == 2  # the two MN9 proboscis motor neurons
    codes = inp.flags & 3
    assert set(np.unique(codes)) == {pack.SIGN_EXC, pack.SIGN_INH, pack.SIGN_NONE}
