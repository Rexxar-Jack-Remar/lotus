# Dynamic Bidirected Dyck Reachability

`CanaryDynamicDyck` maintains one Dyck language on bidirected graphs under
edge insertions and deletions. Public headers are in `include/CFL/DynamicDyck/`;
the namespace is `lotus::cfl::dynamic_dyck`.

## Layout

- `Graph.cpp`, `IO.cpp`: shared graph/parser code and the original file adapter.
- `WeightedQuotient/`: weighted engine, original solver, and cycle-safe deletion.
- `PrimaryComponent/`: primary-component solver and connectivity backends.

Headers and sources mirror the algorithm directories, including
`WeightedQuotient/WeightedQuotientSolver.{h,cpp}` and
`PrimaryComponent/PrimaryComponentSolver.{h,cpp}`. Each directory owns its
CMake source list; both algorithms link through the same library.

## Solvers

- `WeightedQuotientSolver`: Li–Satya–Zhang (POPL 2022), with corrected deletion. Edges are
  set-valued; cyclic deletion may rebuild the graph.
- `PrimaryComponentSolver`: Krishna–Lal–Pavlogiannis–Tuppe (POPL 2024),
  integrated from a supplied implementation, not the authors' artifact.
  Deletion refines affected components without whole-graph Dyck recomputation.
  The library defaults to reference counts; select
  `PrimaryComponentEdgeSemantics::Set` to match `WeightedQuotientSolver`.

Each edge includes its complementary reverse. Labels are parenthesis types,
not epsilon. Access to one solver instance requires external synchronization.

## Build and run

```sh
cmake --build build --target lotus-cfl-dynamic-dyck dynamic_dyck_test \
    dynamic_dyck_primary_component_test dynamic_dyck_primary_component_integration_test \
    dynamic_dyck_primary_component_phases dynamic_dyck_primary_component_allocation_failure \
    dynamic_dyck_primary_component_sparsification
build/bin/lotus-cfl-dynamic-dyck 1 initial.dot updates.seq
build/bin/lotus-cfl-dynamic-dyck --algorithm primary-component --stats initial.dot updates.seq
ctest --test-dir build -R dynamic_dyck --output-on-failure
```

The CLI accepts opaque node IDs and `op--TYPE`/`cp--TYPE` labels. Both algorithms
default to set semantics in the CLI. For primary components, `--counted` enables
reference counts and `--backend hdt` selects the alternative connectivity backend.
Mode `0`/`--recompute` is supported only by the original solver.

See the [full documentation](../../../docs/source/cfl/dynamic_dyck.rst) for API
examples, algorithm qualifications, input formats, and benchmark workflows.
