UseTraceSSA — Ordered Use-History Overlay
=======================================

UseTraceSSA is an analysis-side IR under ``lib/IR/UseTraceSSA``. Public headers
reside under ``include/IR/UseTraceSSA`` and use namespace ``lotus::usetracessa``.
It enriches existing value-flow channels with ordered use histories.

The portable ``CanaryUseTraceSSACore`` target implements history construction,
normalized SVFG import, per-abstract-object resource histories, typed value-flow
edges, source–sink–trap queries, event automata, matched call/return reachability,
witnesses, library summaries, slicing and a structural reachability index.

A use creates a pseudo-version; history phi nodes merge predecessor histories.
Phi operands and successful branch assumptions can be placed on CFG edges.
Local native def-use edges are routed from their consumer's after-use version
so they cannot bypass earlier uses. Pointer addresses, memory contents and
resource state remain separate domains.

Query outcomes are ``Found``, ``NotFound`` and ``Unknown``. A found witness is a
path in the supplied abstraction. Missing models and exhausted search budgets must not become safety claims. May-alias events cannot establish hard sanitizing facts. Structural slices and the SCC index do not establish interprocedural realizability.

Integration status
------------------

``buildUseTraceSSAFromLotusSVFG`` constructs the overlay from Lotus SVFG and
the LLVM module CFG. ``lotus-ir-usetracessa`` first builds ICFG and
SVFG, then exports JSON/DOT, runs a structural query between SVFG node IDs, or
checks possible double-free and use-after-free using object IDs and resource
histories. Ambiguous native locations are reported as graph issues.

``DefectDetector`` is the C++ interface for double-free, use-after-free, taint,
Heartbleed-style and unchecked-use rules. The command-line tool automatically
supplies release and dereference facts for its two resource checks. Taint
sources/sinks, sanitizers and full external-call semantics require client
models. A ``Found`` result means a potential witness in the SVFG abstraction;
it does not prove concrete path feasibility.

See ``lib/IR/UseTraceSSA/README.md`` for the native mapping contract, query
semantics, models, complexity, and full API examples.
