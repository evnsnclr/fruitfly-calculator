"""Readout groups: every neuron belongs to exactly one.

Motor neurons are split into the muscle systems the on-screen fly animates;
everything else is grouped by anatomical superclass so the map can be
coloured and regional firing can be reported.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd


@dataclass(frozen=True)
class Group:
    name: str  # <= 9 chars, shown on the calculator
    rgb: tuple[int, int, int]
    motor: bool = False


GROUPS: tuple[Group, ...] = (
    Group("other", (150, 150, 150)),
    # --- motor pools (order is relied on by calc/ui.c: FS_G_*) ---
    Group("Proboscis", (255, 120, 200), True),  # MN9: rostrum protractor, extends the proboscis
    Group("Mouth", (235, 140, 255), True),  # other proboscis / pharynx motor neurons
    Group("Antenna", (255, 160, 220), True),
    Group("Neck", (255, 190, 150), True),
    Group("Leg F-L", (255, 255, 255), True),
    Group("Leg F-R", (255, 255, 255), True),
    Group("Leg M-L", (255, 255, 255), True),
    Group("Leg M-R", (255, 255, 255), True),
    Group("Leg H-L", (255, 255, 255), True),
    Group("Leg H-R", (255, 255, 255), True),
    Group("Wing L", (255, 240, 120), True),
    Group("Wing R", (255, 240, 120), True),
    Group("Flight pwr", (255, 200, 80), True),
    Group("Jump TTM", (255, 80, 80), True),
    Group("Haltere", (255, 220, 180), True),
    Group("Abdomen", (255, 170, 120), True),
    Group("Motor misc", (230, 230, 230), True),
    # --- everything else, by superclass ---
    Group("Photorec.", (120, 200, 255)),
    Group("Optic", (70, 130, 255)),
    Group("Vis proj", (190, 110, 255)),
    Group("Brain", (255, 215, 80)),
    Group("Descend.", (255, 90, 70)),
    Group("Ascend.", (255, 150, 50)),
    Group("VNC", (90, 235, 130)),
    Group("Sens head", (80, 255, 230)),
    Group("Sens body", (60, 220, 200)),
)

_INDEX = {g.name: i for i, g in enumerate(GROUPS)}
MOTOR_GROUPS = tuple(i for i, g in enumerate(GROUPS) if g.motor)

_SUPERCLASS_GROUP = {
    "ol_sensory": "Photorec.",
    "ol_intrinsic": "Optic",
    "visual_projection": "Vis proj",
    "visual_projection_tbc": "Vis proj",
    "visual_centrifugal": "Vis proj",
    "cb_intrinsic": "Brain",
    "cb_endocrine": "Brain",
    "cb_efferent": "Brain",
    "descending_neuron": "Descend.",
    "descending_neuron_tbc": "Descend.",
    "efferent_descending": "Descend.",
    "ascending_neuron": "Ascend.",
    "efferent_ascending": "Ascend.",
    "vnc_intrinsic": "VNC",
    "vnc_tbc": "VNC",
    "vnc_efferent": "VNC",
    "vnc_endocrine": "VNC",
    "cb_sensory": "Sens head",
    "cb_sensory_tbc": "Sens head",
    "sensory_descending": "Sens head",
    "ENS": "Sens body",
    "vnc_sensory": "Sens body",
    "vnc_sensory_tbc": "Sens body",
    "sensory_ascending": "Sens body",
    "sensory_ascending_tbc": "Sens body",
}

_POWER_TYPES = ("DLMn", "DVMn")
_LEG = {"fl": "Leg F", "ml": "Leg M", "hl": "Leg H"}


def _motor_group(row: pd.Series) -> str:
    sub = row["subclass"] if isinstance(row["subclass"], str) else ""
    typ = row["type"] if isinstance(row["type"], str) else ""
    side = row["side"]
    if typ == "TTMn":
        return "Jump TTM"
    if typ == "MN9":
        return "Proboscis"
    if typ.startswith(_POWER_TYPES):
        return "Flight pwr"
    if sub in _LEG and side in ("L", "R"):
        return f"{_LEG[sub]}-{side}"
    if sub == "wm" and side in ("L", "R"):
        return f"Wing {side}"
    return {"pm": "Mouth", "am": "Antenna", "nm": "Neck", "hm": "Haltere", "ad": "Abdomen"}.get(sub, "Motor misc")


def neuron_side(neurons: pd.DataFrame) -> np.ndarray:
    """'L', 'R' or '' per neuron (soma side, else nerve-root side)."""
    side = neurons["somaSide"].where(neurons["somaSide"].isin(["L", "R"]), neurons["rootSide"])
    return side.where(side.isin(["L", "R"]), "").to_numpy(dtype=object)


def assign_groups(neurons: pd.DataFrame) -> np.ndarray:
    """uint8 group index per neuron."""
    df = neurons.assign(side=neuron_side(neurons))
    out = np.zeros(len(df), np.uint8)
    sc = df["superclass"].to_numpy()
    for name, target in _SUPERCLASS_GROUP.items():
        out[sc == name] = _INDEX[target]
    motor = np.isin(sc, ["vnc_motor", "cb_motor"])
    for i in np.flatnonzero(motor):
        out[i] = _INDEX[_motor_group(df.iloc[i])]
    return out
