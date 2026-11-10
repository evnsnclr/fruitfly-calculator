"""The C core (calculator code compiled for the host, under ASan/UBSan)
against the Python mirror, on a synthetic connectome."""

from __future__ import annotations

import json
import struct
import subprocess
from dataclasses import replace

import numpy as np
import pytest

from pipeline import pack
from pipeline.intsim import IntSim
from pipeline.model import ModelParams

from .conftest import run_cli, sign_of, synthetic_input
from .metafile import header_field_offset, read_meta

pytestmark = pytest.mark.integration


def mirror(
    inp: pack.PackInput,
    scenario: int,
    steps: int,
    side: str = "both",
    rate: int | None = None,
    gain_uv: int | None = None,
    weak: bool = True,
) -> IntSim:
    params = inp.params if gain_uv is None else ModelParams(w_syn_mv=gain_uv / 1000)
    weights = inp.weights
    indptr, targets = inp.indptr, inp.targets
    if not weak:
        rows = np.repeat(np.arange(inp.n), np.diff(indptr))
        keep = weights >= inp.core_min_weight
        indptr = np.concatenate([[0], np.cumsum(np.bincount(rows[keep], minlength=inp.n))]).astype(np.int64)
        targets, weights = targets[keep], weights[keep]
    sim = IntSim(indptr, targets, weights, sign_of(inp.flags), params.fixed(), seed=inp.seed)
    sc = inp.scenarios[scenario]
    ids = {"both": sc.ids, "L": sc.ids[: sc.n_left], "R": sc.ids[sc.n_left : sc.n_left + sc.n_right]}[side]
    sim.set_stimuli([(ids, rate or sc.rate_hz)])
    sim.run(steps)
    return sim


def test_every_row_decodes_to_the_source_graph(synth, host_binaries):
    inp, out, _ = synth
    tiers = pack.split_tiers(inp)
    rng = np.random.default_rng(0)
    rows = np.unique(np.concatenate([np.arange(8), rng.integers(0, inp.n, 60), [inp.n - 1]]))
    for k, (indptr, targets, weights) in enumerate(tiers):
        for r in rows:
            got = run_cli(host_binaries["asan"], out, "--row", str(k), str(r))["edges"]
            a, b = indptr[r], indptr[r + 1]
            assert [e[0] for e in got] == targets[a:b].tolist()
            assert [e[1] for e in got] == weights[a:b].tolist()


@pytest.mark.parametrize("scenario,side,rate", [(0, "both", None), (0, "L", None), (0, "R", 200), (1, "both", 300)])
def test_spike_train_is_bit_exact(synth, host_binaries, scenario, side, rate):
    inp, out, _ = synth
    args = ["--scenario", str(scenario), "--steps", "400", "--side", side]
    if rate:
        args += ["--rate", str(rate)]
    got = run_cli(host_binaries["asan"], out, *args)
    ref = mirror(inp, scenario, 400, side, rate)
    assert ref.total_spikes > 100, "test network is too quiet to be meaningful"
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)
    counts = np.bincount(inp.group_ids, weights=ref.counts, minlength=len(inp.groups)).astype(int)
    assert got["group_counts"] == counts.tolist()


def test_gain_change_matches_mirror(synth, host_binaries):
    inp, out, _ = synth
    got = run_cli(host_binaries["asan"], out, "--scenario", "0", "--steps", "300", "--gain", "1400")
    ref = mirror(inp, 0, 300, gain_uv=1400)
    assert got["theta"] == ref.params.theta
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)


def test_disabling_weak_tier_matches_core_only_graph(synth, host_binaries):
    inp, out, _ = synth
    got = run_cli(host_binaries["asan"], out, "--scenario", "0", "--steps", "300", "--no-weak")
    ref = mirror(inp, 0, 300, weak=False)
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)
    full = mirror(inp, 0, 300)
    assert full.hash != ref.hash, "weak tier should matter in the test network"


def test_paging_does_not_change_results(tmp_path, host_binaries):
    """With too little RAM for the edge data the core must page blocks in
    from storage and still produce the identical spike train."""
    base = synthetic_input(n=60000, seed=11)
    # Drive every neuron so that every block of both tiers is needed.
    everyone = pack.ScenarioInfo("everyone", "all blocks", 40, np.arange(base.n, dtype=np.uint32), 0, 0)
    inp = replace(base, scenarios=(everyone,))
    manifest = pack.write_pack(inp, tmp_path)
    total = sum(t["bytes"] for t in manifest["tiers"])
    assert total > (2 << 20) + (1 << 18), "need well over 2 MB of edges to force paging"
    ref = mirror(inp, 0, 60)
    assert (ref.counts > 0).mean() > 0.8
    free = run_cli(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "60")
    assert free["page_faults"] == 0 and free["resident_bytes"] == free["total_bytes"]
    # 2 MB: one chunk preloaded, one for paging.  1 MB: everything is paged.
    for arena_mb in ("2", "1"):
        tight = run_cli(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "60", "--arena-mb", arena_mb)
        assert tight["page_faults"] > 0 and tight["pool_flushes"] > 0, "paging pool never filled: eviction untested"
        assert tight["resident_bytes"] < tight["total_bytes"] and tight["io_errors"] == 0
        assert tight["chunks"] == int(arena_mb) * 4  # 256 KB chunks
        assert (tight["spikes"], tight["hash"]) == (ref.total_spikes, ref.hash)
    assert (free["spikes"], free["hash"]) == (ref.total_spikes, ref.hash)


def test_silenced_neurons_match_mirror(synth, host_binaries):
    inp, out, _ = synth
    got = run_cli(host_binaries["asan"], out, "--scenario", "0", "--steps", "300", "--silence-every", "3")
    ref = IntSim(inp.indptr, inp.targets, inp.weights, sign_of(inp.flags), inp.params.fixed(), seed=inp.seed)
    ref.silenced[::3] = True
    sc = inp.scenarios[0]
    ref.set_stimuli([(sc.ids, sc.rate_hz)])
    ref.run(300)
    assert ref.total_spikes > 100 and ref.counts[::3].sum() == 0
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)
    assert mirror(inp, 0, 300).hash != ref.hash, "silencing should change the outcome"


def _apply(sim: IntSim, params: ModelParams, ids: np.ndarray, action: str) -> None:
    """One `--at` action of flysim_cli, applied to the dense Python model."""
    name, value = action.split("=")
    if name == "silence":
        sim.silenced[:: int(value)] = True
    elif name == "unsilence":
        sim.silenced[:: int(value)] = False
    elif name == "gain":
        sim.params = replace(params, w_syn_mv=int(value) / 1000).fixed()
    elif name == "rate":
        sim.set_stimuli([(ids, int(value))])
    else:
        raise ValueError(action)


def scripted(inp: pack.PackInput, binary, out, steps: int, script: list[tuple[int, str]], rate: int | None = None):
    """Run the C core and the dense mirror through the same timed changes."""
    args = ["--scenario", "0", "--steps", str(steps)]
    if rate:
        args += ["--rate", str(rate)]
    for step, action in script:
        args += ["--at", str(step), action]
    got = run_cli(binary, out, *args)
    sim = IntSim(inp.indptr, inp.targets, inp.weights, sign_of(inp.flags), inp.params.fixed(), seed=inp.seed)
    ids = inp.scenarios[0].ids
    sim.set_stimuli([(ids, rate or inp.scenarios[0].rate_hz)])
    done = 0
    for step, action in sorted(script, key=lambda e: e[0]):
        sim.run(step - done)
        done = step
        _apply(sim, inp.params, ids, action)
    sim.run(steps - done)
    return got, sim


def test_changes_while_activity_is_in_flight_match_mirror(synth, host_binaries):
    """The core only integrates neurons that could fire and catches the rest
    up when they are next touched.  Changing the threshold or silencing
    neurons in mid-run must not disturb that bookkeeping."""
    inp, out, _ = synth
    script = [
        (40, "silence=3"),
        (70, "gain=1500"),  # threshold drops: sleeping neurons may now be above it
        (100, "unsilence=3"),
        (101, "silence=7"),
        (130, "gain=500"),
        (160, "rate=15"),
        (200, "unsilence=7"),
        (200, "gain=1100"),
        (230, "silence=2"),
        (231, "unsilence=2"),
    ]
    got, ref = scripted(inp, host_binaries["asan"], out, 300, script)
    assert ref.total_spikes > 300
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)
    plain = mirror(inp, 0, 300)
    assert plain.hash != ref.hash


@pytest.mark.parametrize("rate", [2, 8, 40])
def test_long_quiet_gaps_are_caught_up_exactly(synth, host_binaries, rate):
    """Sparse drive: neurons sit untouched for hundreds of steps between
    inputs, so nearly all integration happens in the catch-up path."""
    inp, out, _ = synth
    got, ref = scripted(inp, host_binaries["asan"], out, 2500, [], rate=rate)
    assert ref.total_spikes > 50
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)


def test_only_neurons_that_could_fire_are_stepped(synth, host_binaries):
    inp, out, _ = synth
    got = run_cli(host_binaries["asan"], out, "--scenario", "0", "--steps", "400", "--rate", "8")
    ref = mirror(inp, 0, 400, rate=8)
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)
    # every spike costs timer_start + 1 awake steps at the very least
    assert got["awake_updates"] >= ref.total_spikes * (inp.params.fixed().timer_start + 1)
    assert got["awake_updates"] < 400 * inp.n // 10, "far too many neurons kept awake"
    assert got["catchup_steps"] > 0


def test_phase_timing_uses_the_supplied_clock(tmp_path, host_binaries):
    """With a clock the core splits each step's time into delivery (with the
    flash reads inside it) and integration.  The CLI's fake clock advances
    by one per reading, so the totals count the readings."""
    base = synthetic_input(n=60000, seed=11)
    everyone = pack.ScenarioInfo("everyone", "all blocks", 40, np.arange(base.n, dtype=np.uint32), 0, 0)
    inp = replace(base, scenarios=(everyone,))
    pack.write_pack(inp, tmp_path)
    plain = run_cli(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "40", "--arena-mb", "1")
    got = run_cli(
        host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "40", "--arena-mb", "1", "--fake-clock"
    )
    assert (got["spikes"], got["hash"]) == (plain["spikes"], plain["hash"])
    assert got["page_faults"] > 0
    assert got["ticks_integrate"] == 40
    assert got["ticks_fault"] == got["page_faults"]
    assert got["ticks_deliver"] == 40 + 2 * got["page_faults"]
    assert plain["ticks_deliver"] == plain["ticks_integrate"] == plain["ticks_fault"] == 0


def test_device_benchmark_helpers_are_safe_and_leave_no_trace(synth, host_binaries):
    """The on-calculator benchmark pokes at neural state directly.  Run the
    same calls under the sanitizers and check a later run is unaffected."""
    inp, out, _ = synth
    got = run_cli(host_binaries["asan"], out, "--scenario", "0", "--steps", "300", "--bench-first")
    ref = mirror(inp, 0, 300)
    assert got["bench_edges"] > 0 and got["bench_replayed"] > 0
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)


@pytest.mark.parametrize("tau_s_ms,expect_shift", [(5.0, 2), (10.0, 1), (16.0, 0), (50.0, None)])
def test_other_time_constants_stay_bit_exact(tmp_path, host_binaries, tau_s_ms, expect_shift):
    """The sleep rule depends on the decay constants; slow synapses weaken
    it and finally switch it to 'awake while there is any drive'."""
    inp = synthetic_input(params=ModelParams(w_syn_mv=0.9, tau_s_ms=tau_s_ms))
    pack.write_pack(inp, tmp_path)
    fixed = inp.params.fixed()
    shifts = [sh for sh in range(8, -1, -1) if (fixed.cv << sh) + fixed.dg <= 65536]
    assert (shifts[0] if shifts else None) == expect_shift
    got, ref = scripted(inp, host_binaries["asan"], tmp_path, 400, [(150, "gain=1300"), (250, "silence=4")])
    assert ref.total_spikes > 100
    assert got["bound_shift"] == (expect_shift if expect_shift is not None else 255)
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)


def test_usage_ranking_changes_what_is_pinned_but_not_the_result(tmp_path, host_binaries):
    """Pin by a usage ranking that favours the neurons that actually fire:
    fewer page-ins than index order, identical spikes."""
    base = synthetic_input(n=120000, seed=11)
    ref = mirror(base, 0, 200)
    plain_dir, ranked_dir = tmp_path / "plain", tmp_path / "ranked"
    manifest = pack.write_pack(base, plain_dir)
    assert sum(t["bytes"] for t in manifest["tiers"]) > (4 << 20) + (1 << 19)
    pack.write_pack(replace(base, usage=ref.counts.astype(float)), ranked_dir)
    # 4 MB arena: most of it pinned, about a fifth kept for paging.
    plain = run_cli(host_binaries["asan"], plain_dir, "--scenario", "0", "--steps", "200", "--arena-mb", "4")
    ranked = run_cli(host_binaries["asan"], ranked_dir, "--scenario", "0", "--steps", "200", "--arena-mb", "4")
    for got in (plain, ranked):
        assert got["chunks"] == 16 and got["pool_first"] == 8  # tiny arena: half pinned, half pool
        assert (got["spikes"], got["hash"], got["io_errors"]) == (ref.total_spikes, ref.hash, 0)
    assert ranked["page_faults"] < plain["page_faults"] / 2


def test_corrupt_usage_ranking_is_ignored(tmp_path, host_binaries):
    base = synthetic_input(n=3000, seed=4)
    pack.write_pack(replace(base, usage=np.ones(base.n)), tmp_path)
    meta = read_meta(tmp_path / pack.META_NAME)
    blob = bytearray(meta.raw)
    off = meta.offset_of("HOTS")
    blob[off : off + 4] = b"\xff\xff\xff\xff"  # out-of-range unit, twice
    (tmp_path / pack.META_NAME).write_bytes(bytes(blob))
    got = run_cli(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "100", "--arena-mb", "1")
    ref = mirror(base, 0, 100)
    assert (got["spikes"], got["hash"]) == (ref.total_spikes, ref.hash)


@pytest.mark.parametrize("chunks", [1, 2, 3])
def test_tiny_arenas_still_give_the_exact_result(tmp_path, host_binaries, chunks):
    """One chunk is the single-space pool (plain flush); two and more use the
    two-generation cache.  All must agree with the mirror."""
    inp = synthetic_input(n=20000, seed=6)
    pack.write_pack(inp, tmp_path)
    ref = mirror(inp, 0, 150)
    got = run_cli(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "150", "--arena-chunks", str(chunks))
    assert got["chunks"] == chunks and got["resident_bytes"] < got["total_bytes"]
    assert (got["spikes"], got["hash"], got["io_errors"]) == (ref.total_spikes, ref.hash, 0)
    assert got["page_faults"] > 0


def test_no_stimulus_means_silence(synth, host_binaries):
    _, out, _ = synth
    got = run_cli(host_binaries["asan"], out, "--steps", "200")
    assert got["spikes"] == 0 and got["edges"] == 0


def _copy_with(src, dst, name, mutate):
    for f in src.iterdir():
        (dst / f.name).write_bytes(f.read_bytes())
    blob = bytearray((dst / name).read_bytes())
    mutate(blob)
    (dst / name).write_bytes(bytes(blob))


def _set_u32(offset, value):
    return lambda m: m.__setitem__(slice(offset, offset + 4), struct.pack("<I", value))


def _rejected(binary, data_dir, *args):
    res = subprocess.run([str(binary), str(data_dir), *args], check=False, capture_output=True, text=True, timeout=120)
    assert "AddressSanitizer" not in res.stderr and "runtime error" not in res.stderr, res.stderr[:2000]
    return res


@pytest.mark.parametrize(
    "field,value",
    [
        ("version", 99),
        ("n_blocks", 7),
        ("dt_us", 0),
        ("dt_us", 1 << 30),
        ("seed", 0),
        ("v_thresh_uv", 0),
        ("w_syn_uv", 0),
        ("n_types", 0),
        ("n_types", 0xFFFFFFFF),
        ("n_groups", 0),
        ("dv", 0),
        ("dv", 65536),
        ("dg", 70000),
        ("dg", 65536),
        ("delay", 0),
        ("refrac", 5000),
        ("n_tiers", 3),
        ("n_neurons", 1 << 20),
        ("n_sections", 1 << 28),
    ],
)
def test_bad_header_fields_are_rejected(synth, host_binaries, tmp_path, field, value):
    _, out, _ = synth
    _copy_with(out, tmp_path, pack.META_NAME, _set_u32(header_field_offset(field), value))
    res = _rejected(host_binaries["asan"], tmp_path, "--steps", "5")
    assert res.returncode == 1 and "bad or mismatched data file" in res.stderr, field


@pytest.mark.parametrize(
    "what",
    [
        "magic",
        "truncated",
        "section offset",
        "lut length",
        "lut symbol",
        "wmin",
        "block order",
        "block too big",
        "first block offset",
        "type id",
        "group id",
    ],
)
def test_bad_meta_contents_are_rejected(synth, host_binaries, tmp_path, what):
    _, out, _ = synth
    meta = read_meta(out / pack.META_NAME)
    tier0 = meta.offset_of("TIR0")
    mutate = {
        "magic": lambda m: m.__setitem__(slice(0, 4), b"NOPE"),
        "truncated": lambda m: m.__delitem__(slice(len(m) // 2, len(m))),
        "section offset": _set_u32(104 + 4, len(meta.raw) + 4),
        # LUT entries are (length << 8) | symbol: an 11-bit length and symbol 200 are both impossible
        "lut length": lambda m: m.__setitem__(slice(tier0 + 48, tier0 + 50), struct.pack("<H", (11 << 8) | 1)),
        "lut symbol": lambda m: m.__setitem__(
            slice(tier0 + 48 + 2048, tier0 + 48 + 2050), struct.pack("<H", (3 << 8) | 200)
        ),
        "wmin": _set_u32(tier0 + 32, 1 << 20),
        "block order": _set_u32(tier0 + 3120 + 4, 0xFFFFFFF0),
        "block too big": _set_u32(tier0 + 3120 + 4 * meta.header["n_blocks"], 3 << 20),
        "first block offset": _set_u32(tier0 + 3120, 4),
        "type id": lambda m: m.__setitem__(slice(meta.offset_of("NTYP"), meta.offset_of("NTYP") + 2), b"\xff\xff"),
        "group id": lambda m: m.__setitem__(meta.offset_of("NGRP"), 200),
    }[what]
    _copy_with(out, tmp_path, pack.META_NAME, mutate)
    res = _rejected(host_binaries["asan"], tmp_path, "--steps", "5")
    assert res.returncode == 1 and ("bad or mismatched data file" in res.stderr or "unreadable" in res.stderr), what


def test_truncated_tier_file_is_rejected_at_open(synth, host_binaries, tmp_path):
    _, out, _ = synth
    _copy_with(out, tmp_path, pack.TIER_NAMES[1], lambda m: m.__delitem__(slice(len(m) - 100, len(m))))
    res = _rejected(host_binaries["asan"], tmp_path, "--steps", "5")
    assert res.returncode == 1 and "bad or mismatched data file" in res.stderr


def test_lying_row_lengths_cannot_send_the_decoder_out_of_the_block(synth, host_binaries, tmp_path):
    """The review's proof of concept: every row length in block 0 set to
    0xFFFF used to walk megabytes past the block."""
    _, out, _ = synth
    _copy_with(out, tmp_path, pack.TIER_NAMES[0], lambda m: m.__setitem__(slice(0, 124), b"\xff" * 124))
    res = _rejected(host_binaries["asan"], tmp_path, "--row", "0", "63", "--arena-mb", "1")
    assert res.returncode == 1  # block refused, reported as an I/O error
    res = _rejected(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "100", "--arena-mb", "1")
    assert res.returncode in (0, 1)


def test_overlong_edge_count_stops_at_the_row_end(synth, host_binaries, tmp_path):
    """A row claiming millions of edges must stop when its bytes run out."""
    inp, out, _ = synth
    row0_len = struct.unpack_from("<H", (out / pack.TIER_NAMES[0]).read_bytes(), 0)[0]
    assert row0_len > 8

    def mutate(m):
        m[128:132] = b"\xff\xff\xff\x7f"  # LEB128 count of 2**28 - 1 for row 0

    _copy_with(out, tmp_path, pack.TIER_NAMES[0], mutate)
    res = _rejected(host_binaries["asan"], tmp_path, "--row", "0", "0")
    assert res.returncode == 1
    got = json.loads(_rejected(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "200").stdout)
    assert got["edges"] < 50 * len(inp.targets), "decoder followed a bogus edge count"


@pytest.mark.parametrize("what", ["name offsets", "selftest steps", "few groups", "scenario ids"])
def test_ui_refuses_or_survives_bad_tables(synth, host_binaries, tmp_path, what):
    _, out, _ = synth
    meta = read_meta(out / pack.META_NAME)
    mutate = {
        "name offsets": _set_u32(meta.offset_of("TNAM") + 8, 0x7FFFFFF0),
        "selftest steps": _set_u32(meta.offset_of("TEST") + 4, 0xFFFFFFFF),
        "few groups": _set_u32(header_field_offset("n_groups"), 5),
        "scenario ids": _set_u32(meta.offset_of("SCEN") + 64, 0x7FFFFFFC),
    }[what]
    _copy_with(out, tmp_path, pack.META_NAME, mutate)
    res = subprocess.run(
        [str(host_binaries["preview"]), str(tmp_path), str(tmp_path), "f:3", "t:t", "f:2"],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert "AddressSanitizer" not in res.stderr and "runtime error" not in res.stderr, res.stderr[:2000]
    if what == "selftest steps":
        assert res.returncode == 0 and "selftest FAIL" in res.stdout  # skipped, not run for 4 billion steps
    else:
        assert res.returncode == 1 and "bad or mismatched data file" in res.stderr


def test_missing_tier_file_is_reported(synth, host_binaries, tmp_path):
    _, out, _ = synth
    (tmp_path / pack.META_NAME).write_bytes((out / pack.META_NAME).read_bytes())
    res = subprocess.run([str(host_binaries["asan"]), str(tmp_path)], check=False, capture_output=True, text=True)
    assert res.returncode == 1 and "missing or unreadable" in res.stderr


@pytest.mark.parametrize("hit_headers", [False, True])
def test_corrupt_edge_data_cannot_write_out_of_bounds(synth, host_binaries, tmp_path, hit_headers):
    """Garbage in the edge stream may give wrong spikes, or be refused, but
    must never read or write out of bounds."""
    _, out, _ = synth
    block_off = read_meta(out / pack.META_NAME).tier(0)["block_off"]
    rng = np.random.default_rng(5)

    def mutate(core):
        for pos in rng.integers(0, len(core), 4000):
            in_header = (pos - block_off[np.searchsorted(block_off, pos, side="right") - 1]) < 128
            if in_header == hit_headers or hit_headers:
                core[pos] = int(rng.integers(0, 256))

    _copy_with(out, tmp_path, pack.TIER_NAMES[0], mutate)
    res = _rejected(host_binaries["asan"], tmp_path, "--scenario", "0", "--steps", "300")
    if hit_headers:
        assert res.returncode in (0, 1)  # a damaged row-length table is refused at load
    else:
        assert res.returncode == 0
        got = json.loads(res.stdout)
        assert got["io_errors"] > 0, "corrupt rows should be detected and counted"


def test_ui_runs_headless_under_sanitizers(synth, host_binaries, tmp_path):
    _, out, _ = synth
    script = [
        "f:20",
        "k:tab",
        "f:5",
        "k:tab",
        "f:5",
        "k:down",
        "f:10",
        "k:right",
        "k:plus",
        "t:s",
        "t:w",
        "f:10",
        "k:menu",
        "k:down",
        "k:enter",
        "f:5",
        "t:t",
        "t:t",
        "t:y",
        "k:down",
        "k:enter",
        "f:10",
        "t:x",
        "t:1",
        "k:enter",
        "f:5",
        "t:h",
        "k:esc",
        "t:p",
        "f:3",
        "t:r",
        "k:enter",
        "k:mul",
        "k:div",
        "k:minus",
        "k:left",
        "k:up",
        "f:10",
        "s:final",
    ]
    res = subprocess.run(
        [str(host_binaries["preview"]), str(out), str(tmp_path), *script],
        check=False,
        capture_output=True,
        text=True,
        timeout=600,
    )
    assert res.returncode == 0, res.stderr
    assert "AddressSanitizer" not in res.stderr and "runtime error" not in res.stderr, res.stderr[:2000]
    frame = (tmp_path / "final.ppm").read_bytes()
    assert frame.startswith(b"P6\n320 240\n255\n") and len(frame) == 15 + 320 * 240 * 3
    assert len(set(frame[15:])) > 8, "frame is blank"
