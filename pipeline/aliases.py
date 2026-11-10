"""Short literature names for cell types, taken from the ``synonyms``
annotation (e.g. "Shiu 2022: Usnea" -> "Usnea", "Kennedy and Broadie 2018:
GF" -> "GF").  Shown next to the systematic type name on the calculator and
searchable in the type picker."""

from __future__ import annotations

from collections import Counter

import pandas as pd

MAX_ALIAS = 10


def alias_from_synonyms(text: str) -> str:
    """First synonym's name, or '' if it is not a short plain token."""
    first = text.split(";")[0].split("|")[0]
    name = first.rsplit(":", 1)[-1].strip() if ":" in first else first.strip()
    if not name or len(name) > MAX_ALIAS or not name.isascii() or not name.isprintable() or " " in name:
        return ""
    return name


def type_aliases(neurons: pd.DataFrame, type_names: tuple[str, ...]) -> tuple[str, ...]:
    """Alias per type name ('' when none); the most common one wins."""
    votes: dict[str, Counter] = {}
    typed = neurons[neurons["type"].notna() & neurons["synonyms"].notna()]
    for typ, syn in zip(typed["type"], typed["synonyms"]):
        alias = alias_from_synonyms(syn)
        if alias and alias.lower() != str(typ).lower():
            votes.setdefault(str(typ), Counter())[alias] += 1
    return tuple(max(votes[t].items(), key=lambda kv: (kv[1], kv[0]))[0] if t in votes else "" for t in type_names)
