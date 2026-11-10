"""Paths and dataset constants shared by the pipeline."""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "raw"
CACHE = ROOT / "data" / "cache"
OUT = ROOT / "build" / "calc"

DATASET = "male-cns v1.0 (Janelia FlyEM), minconf 0.5"
BASE_URL = "https://storage.googleapis.com/flyem-male-cns/v1.0/connectome-data/flat-connectome"
ANNOTATIONS = "body-annotations-male-cns-v1.0-minconf-0.5.feather"
NEUROTRANSMITTERS = "body-neurotransmitters-male-cns-v1.0.feather"
WEIGHTS = "connectome-weights-male-cns-v1.0-minconf-0.5.feather"

# Tier split: connections with >= CORE_MIN_WEIGHT synapses are "core" (always
# preferred for RAM residency); single-synapse contacts form the "weak" tier.
CORE_MIN_WEIGHT = 2

# Rows (presynaptic neurons) per on-disk block; must match core/flysim.h.
BLOCK_ROWS = 64
