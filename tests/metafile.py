"""Independent reader for the calculator meta file (test-side only), written
from the C struct definitions in core/flysim.h rather than from pack.py."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

HEADER_FIELDS = [
    "version",
    "n_neurons",
    "n_types",
    "n_groups",
    "n_scenarios",
    "n_blocks",
    "n_tiers",
    "n_edges0",
    "n_edges1",
    "n_syn0",
    "n_syn1",
    "dv",
    "dg",
    "cv",
    "v_thresh_uv",
    "reserved0",
    "w_syn_uv",
    "delay",
    "refrac",
    "dt_us",
    "seed",
    "map_w",
    "map_h",
    "n_sections",
]
HEADER_FORMAT = "<8s11I3i10I"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
TIER_DESC_SIZE = 32 + 4 * 4 + 2 * 1024 + 2 * 512


@dataclass(frozen=True)
class Meta:
    raw: bytes
    header: dict
    sections: dict  # tag -> (offset, size)

    def section(self, tag: str) -> bytes:
        off, size = self.sections[tag]
        return self.raw[off : off + size]

    def offset_of(self, tag: str) -> int:
        return self.sections[tag][0]

    def names(self, tag: str) -> list[str]:
        sec = self.section(tag)
        n = self.header["n_types"]
        offs = np.frombuffer(sec, "<u4", n + 1)
        chars = sec[4 * (n + 1) :]
        return [chars[offs[i] : offs[i + 1]].decode("ascii") for i in range(n)]

    def scenarios(self) -> list[dict]:
        sec = self.section("SCEN")
        out = []
        for k in range(self.header["n_scenarios"]):
            name, hint, rate, n_ids, n_left, n_right, ids_off = struct.unpack_from("<20s28s5I", sec, 68 * k)
            out.append(
                {
                    "name": name.split(b"\0")[0].decode(),
                    "hint": hint.split(b"\0")[0].decode(),
                    "rate_hz": rate,
                    "n_left": n_left,
                    "n_right": n_right,
                    "ids": np.frombuffer(sec, "<u4", n_ids, ids_off),
                }
            )
        return out

    def groups(self) -> list[dict]:
        sec = self.section("GRPS")
        out = []
        for k in range(self.header["n_groups"]):
            name, rgb, flags, size = struct.unpack_from("<12sHHI", sec, 20 * k)
            out.append({"name": name.split(b"\0")[0].decode(), "rgb565": rgb, "motor": bool(flags & 1), "size": size})
        return out

    def selftests(self) -> list[tuple[int, int, int, int]]:
        sec = self.section("TEST")
        return [struct.unpack_from("<4I", sec, 16 * k) for k in range(len(sec) // 16)]

    def tier(self, k: int) -> dict:
        sec = self.section(f"TIR{k}")
        name, wmin, has_w, file_bytes, max_block = struct.unpack_from("<32s4I", sec, 0)
        n_blocks = self.header["n_blocks"]
        return {
            "file": name.split(b"\0")[0].decode(),
            "wmin": wmin,
            "has_weights": bool(has_w),
            "file_bytes": file_bytes,
            "max_block_bytes": max_block,
            "gap_lut": np.frombuffer(sec, "<u2", 1024, 48),
            "w_lut": np.frombuffer(sec, "<u2", 512, 48 + 2048),
            "block_off": np.frombuffer(sec, "<u4", n_blocks + 1, TIER_DESC_SIZE),
        }


def read_meta(path: Path) -> Meta:
    raw = Path(path).read_bytes()
    values = struct.unpack_from(HEADER_FORMAT, raw, 0)
    assert values[0] == b"FLYCNS01"
    header = dict(zip(HEADER_FIELDS, values[1:]))
    sections = {}
    for i in range(header["n_sections"]):
        tag, off, size = struct.unpack_from("<4sII", raw, HEADER_SIZE + 12 * i)
        sections[tag.decode()] = (off, size)
    return Meta(raw, header, sections)


def header_field_offset(name: str) -> int:
    return 8 + 4 * HEADER_FIELDS.index(name)


def c_enum_groups(app_h: Path) -> list[str]:
    """Names of the G_* constants in ui/app.h, in value order (starting at 1)."""
    text = Path(app_h).read_text()
    body = re.search(r"enum\s*\{\s*(G_PROBOSCIS.*?)\}", text, re.DOTALL).group(1)
    return [re.sub(r"\s*=.*", "", tok.strip()) for tok in body.split(",") if tok.strip()]
