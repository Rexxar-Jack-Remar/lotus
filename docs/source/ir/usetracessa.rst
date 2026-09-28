UseTraceSSA — Ordered Use-History Overlay
=======================================

UseTraceSSA is an analysis-side IR under ``lib/IR/UseTraceSSA``. Public headers
reside under ``include/IR/UseTraceSSA`` and use namespace ``lotus::usetracessa``.
It enriches existing value-flow channels with ordered use histories.

The ``CanaryUseTraceSSACore`` target implements history construction,
normalized SVFG import, shared temporal histories with guarded effects, typed value-flow
edges, source–sink–trap queries, event automata, matched call/return reachability,
witnesses, library summaries, slicing and symbolic object-batch queries.

A use creates a pseudo-version; history phi nodes merge predecessor histories.
Phi operands and successful branch assumptions can be placed on CFG edges.
Local native def-use edges are routed from their consumer's after-use version
so they cannot bypass earlier uses. Pointer addresses, memory contents and
resource state remain separate domains.

Query outcomes are ``Found``, ``NotFound`` and ``Unknown``. A found witness is a
path in the supplied abstraction. Missing models and exhausted search budgets must not become safety claims. May-alias events cannot establish hard sanitizing facts. Structural slices do not establish interprocedural realizability.

Integration status
------------------

``buildUseTraceSSAFromLotusSVFG`` constructs the overlay from Lotus SVFG and
the LLVM module CFG. ``lotus-ir-usetracessa`` first builds ICFG and
SVFG, then exports JSON/DOT, runs a structural query between SVFG node IDs, or
checks possible double-free, use-after-free, memory-leak, and file-leak using
object IDs and resource
histories. Ambiguous native locations are reported as graph issues.

``DefectDetector`` is the C++ interface for double-free, use-after-free,
memory-leak, file-leak, taint and unchecked-use rules. The command-line tool
automatically supplies native resource facts for these checks. Taint
sources/sinks, sanitizers and full external-call semantics require client
models. A ``Found`` result means a potential witness in the SVFG abstraction;
it does not prove concrete path feasibility.
Leak rules search for an acquired resource reaching a root exit without a
modeled release. Ownership transfer through globals or containers is not fully
modeled, so these are candidates rather than definitive leak reports.

See ``lib/IR/UseTraceSSA/README.md`` for the native mapping contract, query
semantics, models, complexity, and full API examples.

Object queries
--------------

``Query::memoryObject`` selects one constant abstract object for the whole path.
``QueryEngine::runObjects`` evaluates a finite ``ObjectUniverse`` with LLVM bit
masks and returns separate Found, NotFound and Unknown sets. Both modes retain
product states of the form (flow node, automaton state). Sequential steps and
matched call/return summaries intersect masks; alternative paths union them.
Ordinary taint queries do not acquire same-object semantics.

``TemporalHistory`` represents execution order once per CFG. Resource identity
lives in ``FlowEdge::objects`` and ``GuardedEventEffect`` metadata. TOP guards
apply conservatively as May; known-empty guards apply to no object. Graph size
is independent of the number of objects in a points-to set.
