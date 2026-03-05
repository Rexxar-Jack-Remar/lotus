# IFDS/IDE Include Layout

- **Core/** – Core framework headers (IFDSFramework.h, IFDSIDESolverConfig.h, IFDSIDESolverStatistics.h, EdgeFunctionCache.h, SolverGraphContext.h, SolverRunState.h).
- **Solvers/** – Header-only solver implementations:
  - **IFDSSolver.h** / **IFDSSolver.tpp** – Sequential IFDS solver.
  - **IDESolver.h** / **IDESolver.tpp** – IDE solver.
  - **IterativeIDESolver.h** / **IterativeIDESolver.tpp** – Incremental IDE solver.
  - **PathAwareIFDSSolver.h** – Path-tracking IFDS solver.
  - **PathAwareIDESolver.h** – Path-tracking IDE solver.

## API Notes

- `IDESolver` now exposes path/summary edge query APIs:
  - `get_path_edges(std::vector<PathEdgeType>&)`
  - `get_summary_edges(std::vector<SummaryEdge<Fact>>& )`
- `IDESolver` also exposes unified solver statistics:
  - `get_statistics()`
- `IterativeIDESolver` explicit incremental entry point is:
  - `solve_incremental(const llvm::Module&, const std::set<std::string>& changed_functions)`
- **Utils/** – Utility headers (LLVMFlowHelpers.h).
- **Clients/** – Analysis problem definitions (e.g. IFDSTaintAnalysis, IDEConstantPropagation).

## Include Paths

- Core: `#include "Dataflow/IFDS/Core/IFDSFramework.h"`
- Solvers: `#include "Dataflow/IFDS/Solvers/IFDSSolver.h"`
- Utils: `#include "Dataflow/IFDS/Utils/LLVMFlowHelpers.h"`
- Clients: `#include "Dataflow/IFDS/Clients/IFDSTaintAnalysis.h"`
