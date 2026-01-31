# IFDS/IDE Include Layout

- **IFDSFramework.h** – Core framework (path edges, summary edges, problem interfaces).
- **Solvers/** – Header-only solver implementations:
  - **IFDSSolver.h** / **IFDSSolver.tpp** – Sequential IFDS solver.
  - **IDESolver.h** / **IDESolver.tpp** – IDE solver.
- **Clients/** – Analysis problem definitions (e.g. IFDSTaintAnalysis, IDEConstantPropagation).

Use `Dataflow/IFDS/Solvers/IFDSSolver.h` or `Dataflow/IFDS/Solvers/IDESolver.h` for solvers.
