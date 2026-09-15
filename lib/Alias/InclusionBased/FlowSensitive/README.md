# Flow-sensitive inclusion-based pointer analysis

`FlowSensitivePTA` is the thread-independent sparse solver. It maintains:

- top-level points-to sets for pointer-producing SVFG nodes;
- per-node, per-object MemorySSA `IN` and `OUT` points-to state;
- explicit Addr/Copy/GEP/Phi/Load/Store and parameter-flow transfer;
- canonical `(allocation, normalized byte offset)` field objects, with array
  indices collapsed for field-insensitive updates;
- field-offset-aware aggregate global initializers and `memcpy`/`memmove`;
- singleton-object strong updates and conservative weak updates;
- Tarjan SCC decomposition with SCC-local fixed points and successor requeueing;
- auxiliary-PTA call-graph initialization plus on-the-fly indirect-call
  connection followed by SCC reconstruction;
- selectable mutable and hash-consed points-to set storage.

The concurrency layer does not duplicate this solver. `FSMPTA` runs it over an
SVFG augmented with fork/join and `ThreadMHPIndirectVF` edges. MSli supplies an
optional filtered solve graph.

This module contains three flow-sensitive analyses:

- `FlowSensitivePTA` implements the default exhaustive `fspta` analysis.
- `VersionedFlowSensitivePTA` implements `vfspta` object prelabeling, meld
  versions, consume/yield maps, version and statement reliance, strong and weak
  updates, intrinsic memory definitions, footprint-equivalent object reuse,
  occurrence-weighted propagation, OTF delta-edge updates, and result
  persistence.
- `ValueFlowPTA` implements the value-flow formulation of Li, Cifuentes, and
  Keynes (ESEC/FSE 2011). It builds a field-insensitive value-flow graph
  directly from LLVM IR, orders indirect-flow construction by object escape,
  and applies Algorithm 2's store-plus-IDF sparse reaching-definition solve to
  its interprocedural supergraph. It does not require a precomputed SVFG or
  flow-insensitive pre-analysis.

Select the versioned solver with `lotus-alias-fspta --analysis=vfspta`.
Select the direct value-flow solver with
`lotus-alias-fspta --analysis=vfpta`.
