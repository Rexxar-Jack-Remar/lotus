# MPI Static Analysis

This module provides static analysis for MPI programs in LLVM IR. It focuses on
communication structure in the SPMD model rather than shared-memory threading.

## Components

- `MPIProcessModel`: extracts MPI operations and records metadata such as
  communicator, rank, tag, request, and window handles.
- `MPICollectiveAnalysis`: checks collective compatibility and flags
  rank-guarded collectives.
- `MPIRMAAnalysis`: tracks RMA windows, synchronization epochs, and possible
  RMA races.
- `MPIRankAnalysis`: symbolic rank reasoning used by the collective checker.

## Entry Point

Use [MPIAnalysis.h](lotus/include/Analysis/Concurrency/MPI/MPIAnalysis.h):

```c++
mpi::MPIAnalysis analysis(module);
analysis.runAnalysis();
const auto &results = analysis.getResults();
```

The top-level results include:

- orphaned non-blocking requests
- potential blocking send/recv deadlocks
- mismatched collectives
- conditional collectives
- unsynchronized RMA operations
- potential RMA races
- leaked windows

## Supported Modeling

- Point-to-point: blocking and non-blocking send/recv, plus `MPI_Sendrecv`
- Collectives: barriers and common blocking/non-blocking collectives
- Requests: `Wait*`, `Test*`, `Request_free`, `Cancel`
- Symbol aliases: `PMPI_*`, `__wrap_MPI_*`, and OpenMPI internal
  `ompi_mpi_*` symbols are normalized to MPI semantics
- Communicators: basic alias/canonicalization support for duplicated or split
  communicators
- RMA: `Put`, `Get`, `Accumulate`, selected atomic ops, and lock/fence-style
  synchronization

## Limitations

- Deadlock detection is intentionally lightweight and targets simple blocking
  cycles.
- Collective checking is sequence-based, not a full path-sensitive protocol
  proof.
- Unknown ranks, tags, or communicators are handled conservatively.
- PSCW RMA synchronization is recognized but not treated as a supported proof
  of synchronization.

## Tests

See:

- `tests/unit/Concurrency/MPIAnalysisTest.cpp`
- `tests/unit/Concurrency/MPIRankAnalysisTest.cpp`

## Related Work

- MC-CChecker, EuroMPI 2018
- MC-Checker, SC 2014
- Dynamic Data Race Detection for MPI-RMA Programs, EuroMPI 2021
