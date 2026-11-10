from __future__ import annotations

from dataclasses import replace

import numpy as np
import pytest

from pipeline import pack
from pipeline.aliases import alias_from_synonyms
from pipeline.groups import GROUPS
from pipeline.model import FRAC, ModelParams

from .conftest import ROOT, synthetic_input
from .metafile import HEADER_SIZE, c_enum_groups, read_meta

pytestmark = pytest.mark.unit


def test_meta_file_reads_back_field_for_field(synth):
    inp, out, manifest = synth
    meta = read_meta(out / pack.META_NAME)
    h, fx = meta.header, inp.params.fixed()
    core, weak = pack.split_tiers(inp)
    assert HEADER_SIZE == 104
    assert (h["version"], h["n_neurons"], h["n_types"], h["n_groups"], h["n_scenarios"], h["n_tiers"]) == (
        pack.VERSION,
        inp.n,
        len(inp.type_names),
        len(inp.groups),
        len(inp.scenarios),
        2,
    )
    assert h["n_blocks"] == -(-inp.n // 64)
    assert (h["n_edges0"], h["n_edges1"]) == (len(core[1]), len(weak[1]))
    assert (h["n_syn0"], h["n_syn1"]) == (int(core[2].sum()), int(weak[2].sum()))
    assert (h["dv"], h["dg"], h["cv"], h["delay"], h["refrac"], h["dt_us"]) == (
        fx.dv,
        fx.dg,
        fx.cv,
        fx.delay,
        fx.refrac,
        fx.dt_us,
    )
    assert (h["v_thresh_uv"], h["w_syn_uv"], h["seed"]) == (inp.params.v_thresh_uv, inp.params.w_syn_uv, inp.seed)
    assert (h["map_w"], h["map_h"]) == (inp.map_w, inp.map_h)
    for tag, (off, size) in meta.sections.items():
        assert off % 4 == 0 and off + size <= len(meta.raw), tag
    assert np.array_equal(np.frombuffer(meta.section("NFLG"), "u1", inp.n), inp.flags)
    assert np.array_equal(np.frombuffer(meta.section("NPIX"), "<u2", inp.n), inp.pix)
    assert np.array_equal(np.frombuffer(meta.section("NTYP"), "<u2", inp.n), inp.type_ids)
    assert np.array_equal(np.frombuffer(meta.section("NGRP"), "u1", inp.n), inp.group_ids)
    assert meta.names("TNAM") == list(inp.type_names)
    assert meta.names("TALI") == list(inp.type_aliases)
    assert np.array_equal(
        np.frombuffer(meta.section("TCNT"), "<u2", h["n_types"]), np.bincount(inp.type_ids, minlength=h["n_types"])
    )
    for got, want, size in zip(meta.groups(), inp.groups, np.bincount(inp.group_ids, minlength=len(inp.groups))):
        assert got == {"name": want.name, "rgb565": pack.rgb565(want.rgb), "motor": want.motor, "size": int(size)}
    for got, want in zip(meta.scenarios(), inp.scenarios):
        assert (got["name"], got["hint"], got["rate_hz"], got["n_left"], got["n_right"]) == (
            want.name,
            want.hint,
            want.rate_hz,
            want.n_left,
            want.n_right,
        )
        assert np.array_equal(got["ids"], want.ids)
    assert meta.selftests() == [tuple(t) for t in inp.selftests]
    for k, tier in enumerate(manifest["tiers"]):
        desc = meta.tier(k)
        data = (out / desc["file"]).read_bytes()
        assert (desc["file"], desc["file_bytes"], desc["wmin"], desc["has_weights"]) == (
            tier["file"],
            len(data),
            tier["wmin"],
            tier["has_weights"],
        )
        assert desc["block_off"][0] == 0 and desc["block_off"][-1] == len(data)
        assert (np.diff(desc["block_off"].astype(np.int64)) >= 128).all()
        assert desc["max_block_bytes"] == np.diff(desc["block_off"].astype(np.int64)).max()
    assert manifest["edges_total"] == len(inp.targets)
    assert manifest["synapses_total"] == int(inp.weights.sum())


def test_group_table_matches_the_ui_enum():
    """ui/app.h indexes groups by constant; the two lists must stay in step."""
    expected = {
        "G_PROBOSCIS": "Proboscis",
        "G_MOUTH": "Mouth",
        "G_ANTENNA": "Antenna",
        "G_NECK": "Neck",
        "G_LEG_FL": "Leg F-L",
        "G_LEG_FR": "Leg F-R",
        "G_LEG_ML": "Leg M-L",
        "G_LEG_MR": "Leg M-R",
        "G_LEG_HL": "Leg H-L",
        "G_LEG_HR": "Leg H-R",
        "G_WING_L": "Wing L",
        "G_WING_R": "Wing R",
        "G_FLIGHT": "Flight pwr",
        "G_JUMP": "Jump TTM",
        "G_HALTERE": "Haltere",
        "G_ABDOMEN": "Abdomen",
        "G_MOTOR_MISC": "Motor misc",
        "G_PHOTO": "Photorec.",
        "G_OPTIC": "Optic",
        "G_VISPROJ": "Vis proj",
        "G_BRAIN": "Brain",
        "G_DESC": "Descend.",
        "G_ASC": "Ascend.",
        "G_VNC": "VNC",
        "G_SENS_HEAD": "Sens head",
        "G_SENS_BODY": "Sens body",
    }
    enum = c_enum_groups(ROOT / "ui" / "app.h")
    assert enum == list(expected), "enum order changed"
    assert [g.name for g in GROUPS[1:]] == [expected[name] for name in enum]
    assert all(len(g.name) <= 11 for g in GROUPS)  # char name[12] on disk
    assert all(len(g.name) <= 9 for g in GROUPS[18:])  # region rows show the name in 9 columns
    assert [g.motor for g in GROUPS[1:18]] == [True] * 17 and not any(g.motor for g in GROUPS[18:])


def test_tiers_partition_the_edges(synth):
    inp, _, manifest = synth
    core, weak = pack.split_tiers(inp)
    assert len(core[1]) + len(weak[1]) == len(inp.targets)
    assert core[2].min() >= inp.core_min_weight and (weak[2] == 1).all()
    assert [t["edges"] for t in manifest["tiers"]] == [len(core[1]), len(weak[1])]
    assert manifest["tiers"][1]["has_weights"] is False


@pytest.mark.parametrize(
    "change,message",
    [
        (lambda i: replace(i, flags=i.flags[:-1]), "length n"),
        (lambda i: replace(i, seed=0), "seed"),
        (lambda i: replace(i, pix=np.full(i.n, 180 * 240, np.uint16)), "pixel"),
        (lambda i: replace(i, type_ids=np.full(i.n, 4000, np.uint16)), "out of range"),
        (lambda i: replace(i, weights=np.zeros_like(i.weights)), "weights"),
        (lambda i: replace(i, type_aliases=i.type_aliases[:-1]), "parallel"),
    ],
)
def test_invalid_input_is_rejected(tmp_path, change, message):
    with pytest.raises(ValueError, match=message):
        pack.write_pack(change(synthetic_input(n=300)), tmp_path)


def test_overflow_guard_trips_on_absurd_convergence(tmp_path):
    inp = synthetic_input(n=300)
    huge = replace(inp, weights=np.full_like(inp.weights, 3_000_000))
    with pytest.raises(ValueError, match="overflow"):
        pack.check_no_overflow(huge)
    assert pack.check_no_overflow(inp) < 2**31


def test_overflow_bound_covers_decay_plus_frozen_refractory_input():
    inp = synthetic_input(n=300)
    total_in = np.zeros(inp.n, np.int64)
    np.add.at(total_in, inp.targets, inp.weights)
    per_step = int(total_in.max()) << FRAC
    steady = per_step / (1.0 - inp.params.decay_g)
    bound = pack.check_no_overflow(inp)
    assert bound >= steady + inp.params.refrac_steps * per_step
    assert bound <= steady + (inp.params.refrac_steps + 2) * per_step


@pytest.mark.parametrize(
    "change,message",
    [
        (lambda i: replace(i, targets=np.where(np.arange(len(i.targets)) == 0, i.n, i.targets)), "outside 0..n-1"),
        (lambda i: replace(i, scenarios=(replace(i.scenarios[0], ids=np.array([i.n], np.uint32)),)), "outside 0..n-1"),
    ],
)
def test_out_of_range_indices_are_rejected(tmp_path, change, message):
    with pytest.raises(ValueError, match=message):
        pack.write_pack(change(synthetic_input(n=300)), tmp_path)


def test_long_names_are_rejected(tmp_path):
    inp = synthetic_input(n=300)
    bad = replace(inp, scenarios=(replace(inp.scenarios[0], name="x" * 20),))
    with pytest.raises(ValueError, match="too long"):
        pack.write_pack(bad, tmp_path)


def test_rgb565():
    assert pack.rgb565((255, 255, 255)) == 0xFFFF and pack.rgb565((0, 0, 0)) == 0
    assert pack.rgb565((255, 0, 0)) == 0xF800 and pack.rgb565((0, 0, 255)) == 0x001F


@pytest.mark.parametrize(
    "text,alias",
    [
        ("Shiu 2022: Usnea", "Usnea"),
        ("Kennedy and Broadie 2018: GF", "GF"),
        ("Lee 2002, Rideout 2010, Nojima 2021: pC1; Cachero 2010: pMP-e", "pC1"),
        ("Shiu 2022: Billiards | Shiu 2022: Specter", "Billiards"),
        ("Yao & Scott 2022: Sugar SEL PN", ""),  # spaces: not a compact token
        ("Someone 2020: Averyveryverylongname", ""),
        ("mAL", "mAL"),
    ],
)
def test_alias_extraction(text, alias):
    assert alias_from_synonyms(text) == alias


def test_default_model_is_the_shiu_parameter_set():
    p = ModelParams()
    assert (p.tau_m_ms, p.tau_s_ms, p.v_thresh_mv, p.t_refrac_ms, p.t_delay_ms) == (20.0, 5.0, 7.0, 2.2, 1.8)
    assert p.w_syn_mv == 0.275


def test_hot_unit_order_ranks_by_usage_per_byte():
    block_offs = [np.array([0, 1000, 3000, 3100], np.uint32), np.array([0, 500, 600, 2600], np.uint32)]
    usage = np.zeros(3 * 64)
    usage[0] = 10.0  # block 0
    usage[64] = 10.0  # block 1
    order = pack.hot_unit_order(usage, block_offs).tolist()
    # densities: t0b0 10/1000, t0b1 10/2000, t1b0 10/500, t1b1 10/100; unused blocks last, core tier first
    assert order == [(1 << 15) | 1, (1 << 15) | 0, 0, 1, 2, (1 << 15) | 2]
    assert sorted(order) == sorted([0, 1, 2, (1 << 15), (1 << 15) | 1, (1 << 15) | 2])


def test_usage_ranking_is_written_and_validated(tmp_path):
    inp = synthetic_input(n=300)
    rng = np.random.default_rng(1)
    manifest = pack.write_pack(replace(inp, usage=rng.random(inp.n)), tmp_path)
    meta = read_meta(tmp_path / pack.META_NAME)
    hot = np.frombuffer(meta.section("HOTS"), "<u2")
    n_blocks = manifest["blocks"]
    assert len(hot) == 2 * n_blocks
    assert sorted(hot.tolist()) == sorted(list(range(n_blocks)) + [(1 << 15) | b for b in range(n_blocks)])
    with pytest.raises(ValueError, match="usage"):
        pack.write_pack(replace(inp, usage=np.zeros(5)), tmp_path)
