# WPDS-based Interprocedural Dataflow Analysis Framework

A framework for interprocedural may analyses using Weighted Pushdown Systems
(WPDS) in LLVM IR.

## Overview

WPDS is a solver for distributive data-flow problems, very similar to IDE
(Interprocedural Distributive Environment). This implementation currently
supports may analyses over finite Value* fact sets. Instead of building an
exploded super-graph using user-provided flow and edge functions, a weighted
pushdown system is built whose rules are drawn from the user's analysis
description. The analysis problem is solved using a stack automaton obtained by
the post* or pre* algorithm using the legacy `third-party/WPDS` implementation
or the optional vendored WALi implementation.

## Directory Structure

```
include/Dataflow/WPDS/
├── Backend/                       # Neutral model/session interfaces
├── Core/                          # Public fact/weight/builder abstractions
├── Solver/                        # Public engine interface
├── Analyses/                       # Public client analysis wrappers
└── InterProceduralDataFlow.h      # Umbrella header

lib/Dataflow/WPDS/
├── Backend/                       # Legacy and optional WALi adapters
├── Core/                          # Core abstractions
│   ├── DataFlowFacts.cpp         # Fact domain implementation
│   └── GenKillTransformer.cpp   # Semiring weight implementation
├── Solver/                       # Fixpoint solvers
│   └── InterProceduralDataFlowEngine.cpp  # WPDS solver engine
├── Analyses/                       # Client analysis implementations
│   ├── TaintAnalysis.cpp
│   ├── ConstantPropagationAnalysis.cpp
│   ├── LivenessAnalysis.cpp
│   └── UninitializedVariablesAnalysis.cpp
├── CMakeLists.txt
└── README.md
```

## Quick Start

To write a WPDS analysis, you essentially write an IDE analysis. The framework provides:

1. **Define flow functions** using `GenKillTransformer` for gen/kill-style dataflow
2. **Use `MemoryObjectFact`** when facts should represent canonical memory objects
3. **Run the analysis** using `InterProceduralDataFlowEngine`

Modern include paths:

- `#include "Dataflow/WPDS/Core/DataFlowFacts.h"`
- `#include "Dataflow/WPDS/Core/GenKillTransformer.h"`
- `#include "Dataflow/WPDS/Solver/InterProceduralDataFlowEngine.h"`

Or use the umbrella header:

- `#include "Dataflow/WPDS/InterProceduralDataFlow.h"`

## Current Limits

- `GEN`/`KILL` in `mono::DataFlowResult` are local instruction effects only.
- `IN`/`OUT` are concrete fact sets for the selected initial automaton/query.
- External and indirect calls are conservatively approximated and can be
  customized with `ExternalCallPolicy`.
- Memory is modeled coarsely via canonical base objects; precise field-sensitive
  or alias-aware strong updates are not supported.
- Must analyses are not supported by the current `GenKillTransformer`.
- Higher-level point-query helpers are available; raw regular-language queries
  remain an expert API and may be more expensive than direct point queries.

## Supported semantics

- The current framework targets distributive may analyses over a finite fact
  domain.
- `GEN` and `KILL` are local transfer effects of the current instruction, not
  accumulated path summaries.
- Memory objects are tracked via canonical base objects such as allocas,
  globals, pointer arguments, and pointer-returning calls.

```cpp
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

wpds::InterProceduralDataFlowEngine Engine;
auto Result = Engine.runForwardAnalysis(
    M,
    [](Instruction* inst) { return /* transformer */; },
    {/* initial facts */}
);

auto FactsAfter = Engine.queryFactsAfterInstruction(SomeInst);
auto SummaryBefore = Engine.querySummaryBeforeInstruction(SomeInst);
```

## Selectable solver backends

The legacy solver remains the default. Configure with
`-DLOTUS_ENABLE_WALI_OPENNWA=ON` to enable the vendored WALi implementations,
then select one programmatically:

```cpp
wpds::WPDSBackendOptions options;
options.backend = wpds::WPDSBackendKind::WaliSWPDS;
options.verifyAgainstLegacy = true;

wpds::InterProceduralDataFlowEngine engine(options);
auto prepared = engine.prepareForwardAnalysis(module, createTransformer);
auto first = prepared->solve(seedA);
auto second = prepared->solve(seedB); // reuses the prepared model
auto contexts = prepared->solveContextAggregated(seedA);
```

`solve` retains the legacy exact-stack observation. The context-aggregated form
matches a program-point symbol followed by any call-stack suffix. Explicitly
requesting an unavailable backend or passing a legacy configuration-automaton
callback to a WALi backend returns `nullptr` and records an actionable message
in `getLastError()`.

The four existing client functions have compatible overloads taking
`WPDSBackendOptions`. The `lotus-dfa-wpds` tool accepts:

```text
--analysis=liveness|constant_prop|taint|uninitialized
--wpds-backend=legacy|wali-fwpds|wali-swpds
--wpds-stats
--wpds-verify-against-legacy
--wpds-query-count=N
--summary-only
```

Query batches currently use a prepared liveness session and deterministic,
distinct boundary seeds drawn from the module. The other client analyses
support runtime backend selection for one-shot runs. See
[BACKEND_AUDIT.md](BACKEND_AUDIT.md) for the lowering, query, ownership, SWPDS
lifecycle, concurrency, and compatibility contracts.

## References

- Reps, Schwoon, Jha, Melski: "Weighted pushdown systems and their application to interprocedural dataflow analysis" (SAS 2005)
- Lal, Reps: "Improving Pushdown System Model Checking" (CAV 2006)
- Lal, Reps, Balakrishnan: "Extended Weighted Pushdown Systems" (CAV 2005)
