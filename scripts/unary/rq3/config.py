"""Configuration for the RQ3 upper-approximation comparison."""

from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]

INTERLEAVED_DYCK_BINARY = (
    REPOSITORY_ROOT / "build/bin/lotus-cfl-interleaved-dyck"
)
BINARIES = {
    "staged": INTERLEAVED_DYCK_BINARY,
    "mcfl": INTERLEAVED_DYCK_BINARY,
    "unary": INTERLEAVED_DYCK_BINARY,
    "spds": INTERLEAVED_DYCK_BINARY,
}

# Compare Regularization, Mutual Refinement, and ACF on both suites.
# SPDS uses the corresponding taint/value-flow client on both suites.
BENCHMARK_SUITES = [
    {
        "name": "taint",
        "directory": REPOSITORY_ROOT
        / "benchmarks/real-world/CFL/InterleavedDyck/taint",
        "analysis": "taint",
        "enabled": True,
    },
    {
        "name": "valueflow",
        "directory": REPOSITORY_ROOT
        / "benchmarks/real-world/CFL/InterleavedDyck/valueflow",
        "analysis": "value-flow",
        "enabled": True,
    },
]
DOT_GLOB = "*.dot"
SEARCH_RECURSIVELY = True

# Arguments may use {analysis}, which is replaced from BENCHMARK_SUITES.
# artifact_kind controls the output from the separate relation-capture run.
# MCFL remains configured for future experiments but
# is disabled because it does not yet implement the value-flow endpoint model.
EXPERIMENTS = [
    {
        "name": "spds",
        "tool": "spds",
        "args": ["spds", "--analysis", "{analysis}", "--all-pairs"],
        "analyses": ["taint", "value-flow"],
        "artifact_kind": "upper",
        "enabled": True,
    },
    {
        "name": "regularization",
        "tool": "staged",
        "args": ["staged-bounds", "--analysis", "{analysis}",
                 "--method", "regularization"],
        "artifact_kind": "upper",
        "enabled": True,
    },
    {
        "name": "mutual-refinement",
        "tool": "staged",
        "args": ["staged-bounds", "--analysis", "{analysis}",
                 "--method", "mutual-refinement"],
        "artifact_kind": "upper",
        "enabled": True,
    },
    # Excluded from RQ3: standalone lower bound and the full on-demand pipeline.
    {
        "name": "union-dyck",
        "tool": "staged",
        "args": [
            "staged-bounds",
            "--analysis",
            "{analysis}",
            "--method",
            "underapproximation",
        ],
        "artifact_kind": "union",
        "enabled": False,
    },
    {
        "name": "staged-on-demand",
        "tool": "staged",
        "args": [
            "staged-bounds",
            "--analysis",
            "{analysis}",
            "--method",
            "all",
            "--parity-groups",
            "2",
        ],
        "artifact_kind": "staged",
        "enabled": False,
    },
    {
        "name": "acf",
        "tool": "unary",
        "args": ["unary", "--algorithm", "adaptive", "--bidirect", "--stats"],
        "artifact_kind": "components",
        "enabled": True,
    },
    {
        "name": "mcfl-plus-d1",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "1", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 1,
        "enabled": False,
    },
    {
        "name": "mcfl-plus-d2",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "2", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 2,
        "enabled": False,
    },
    {
        "name": "mcfl-plus-d3",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "3", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 3,
        "required": False,
        "enabled": False,
    },
    {
        "name": "mcfl-circ-d1",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "1", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 1,
        "enabled": False,
    },
    {
        "name": "mcfl-circ-d2",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "2", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 2,
        "enabled": False,
    },
    {
        "name": "mcfl-circ-d3",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "3", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 3,
        "required": False,
        "enabled": False,
    },
]

WARMUP_REPETITIONS = 0
MEASURED_REPETITIONS = 5
TIMEOUT_SECONDS = 3600
# RLIMIT_AS per process on Linux/macOS.  Set to None to disable it.
MEMORY_LIMIT_GIB = 128
STOP_REPETITIONS_AFTER_FAILURE = True
CAPTURE_RELATIONS = True

DATA_DIRECTORY = Path(__file__).resolve().parent / "data"
OUTPUT_CSV = DATA_DIRECTORY / "rq3-runs.csv"
BOUNDS_CSV = DATA_DIRECTORY / "rq3-bounds.csv"
METHODS_CSV = DATA_DIRECTORY / "rq3-methods.csv"
LOG_DIRECTORY = DATA_DIRECTORY / "log"
ARTIFACT_DIRECTORY = DATA_DIRECTORY / "artifacts"
RESUME_EXISTING_CSV = True

EXTRA_ENVIRONMENT = {}


def supports_analysis(experiment, analysis):
    return analysis in experiment.get("analyses", ["taint", "value-flow"])
