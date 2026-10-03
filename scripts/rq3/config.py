"""Configuration for the RQ3 interleaved-Dyck bounds experiment."""

from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]

BINARIES = {
    "staged": REPOSITORY_ROOT
    / "build/bin/lotus-cfl-interleaved-dyck-staged-bounds",
    "mcfl": REPOSITORY_ROOT / "build/bin/lotus-cfl-interleaved-dyck-mcfl",
    "unary": REPOSITORY_ROOT / "build/bin/lotus-cfl-interleaved-dyck-unary",
}

# Value-flow uses an analysis-specific endpoint language in StagedBounds.  Keep
# it disabled until the same query universe is explicitly applied to MCFL and
# ACF.  Enabling a suite is otherwise the only dataset-policy change needed by
# the runner.
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
        "enabled": False,
    },
]
DOT_GLOB = "*.dot"
SEARCH_RECURSIVELY = True

# Arguments may use {analysis}, which is replaced from BENCHMARK_SUITES.
# artifact_kind controls the machine-readable output captured on the first
# successful measured run.  G_3 variants are attempted under the same timeout
# and memory limits; a timeout or memory failure records their exclusion.
EXPERIMENTS = [
    {
        "name": "union-dyck",
        "tool": "staged",
        "args": [
            "--analysis",
            "{analysis}",
            "--method",
            "underapproximation",
        ],
        "artifact_kind": "union",
        "enabled": True,
    },
    {
        "name": "staged-on-demand",
        "tool": "staged",
        "args": [
            "--analysis",
            "{analysis}",
            "--method",
            "all",
            "--parity-groups",
            "2",
        ],
        "artifact_kind": "staged",
        "enabled": True,
    },
    {
        "name": "acf",
        "tool": "unary",
        "args": ["--algorithm", "adaptive", "--bidirect", "--stats"],
        "artifact_kind": "components",
        "enabled": True,
    },
    {
        "name": "mcfl-plus-d1",
        "tool": "mcfl",
        "args": ["--dimension", "1", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 1,
        "enabled": True,
    },
    {
        "name": "mcfl-plus-d2",
        "tool": "mcfl",
        "args": ["--dimension", "2", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 2,
        "enabled": True,
    },
    {
        "name": "mcfl-plus-d3",
        "tool": "mcfl",
        "args": ["--dimension", "3", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 3,
        "required": False,
        "enabled": True,
    },
    {
        "name": "mcfl-circ-d1",
        "tool": "mcfl",
        "args": ["--dimension", "1", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 1,
        "enabled": True,
    },
    {
        "name": "mcfl-circ-d2",
        "tool": "mcfl",
        "args": ["--dimension", "2", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 2,
        "enabled": True,
    },
    {
        "name": "mcfl-circ-d3",
        "tool": "mcfl",
        "args": ["--dimension", "3", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 3,
        "required": False,
        "enabled": True,
    },
]

WARMUP_REPETITIONS = 0
MEASURED_REPETITIONS = 5
TIMEOUT_SECONDS = 3600
# RLIMIT_AS per process on Linux/macOS.  Set to None to disable it.
MEMORY_LIMIT_GIB = 128
STOP_REPETITIONS_AFTER_FAILURE = True
CAPTURE_RELATIONS = True

OUTPUT_CSV = REPOSITORY_ROOT / "scripts/rq3/rq3-runs.csv"
BOUNDS_CSV = REPOSITORY_ROOT / "scripts/rq3/rq3-bounds.csv"
METHODS_CSV = REPOSITORY_ROOT / "scripts/rq3/rq3-methods.csv"
LOG_DIRECTORY = REPOSITORY_ROOT / "scripts/rq3/log"
ARTIFACT_DIRECTORY = REPOSITORY_ROOT / "scripts/rq3/artifacts"
RESUME_EXISTING_CSV = True

EXTRA_ENVIRONMENT = {}
