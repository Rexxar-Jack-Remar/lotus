Alias Analysis Tools
====================

This page documents the command-line tools under ``tools/alias/``. For the
underlying algorithms and architecture, see :doc:`../../analysis/alias_analysis`.

SparrowAA (sparrow-aa)
-------------------

Inclusion-based points-to analysis (flow-insensitive, context-insensitive, context-sensitive).

**Binary**: ``sparrow-aa``  
**Location**: ``tools/alias/sparrow-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/sparrow-aa [options] input.bc

**Notes**:

- Flow-insensitive, context-insensitive, context-sensitive
- No on-the-fly call graph construction
- Good default when you need quick alias information
- Note: this tool have some redundancies with aserpta, and reuses some header files from it (from context abstraction).

TPA (tpa)
---------

Flow- and context-sensitive pointer analysis using semi-sparse representation
with k-limiting support.

**Binary**: ``tpa``  
**Location**: ``tools/alias/tpa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/tpa [options] input.bc

**Key Options** (see also :doc:`../../alias/tpa`):

- ``-ext <file>`` – External pointer table file for modeling library functions
- ``-no-prepass`` – Skip TPA IR normalization prepasses (GEP expansion, etc.)
- ``-prepass-out <file>`` – Write module after prepass to file (suffix .ll or .bc)
- ``-cfg-dot-dir <dir>`` – Write per-function pointer CFGs as .dot files into directory
- ``-print-pts`` – Print points-to sets for pointers materialized by the analysis
- ``-print-indirect-calls`` – Print resolved targets for each indirect call

**Characteristics**:

- Flow-sensitive: tracks control flow within functions
- Context-sensitive: supports k-limiting and adaptive context strategies
- Semi-sparse representation: only analyzes def-use chains, not all program points
- Field-sensitive memory model: models individual struct fields and array elements

**Examples**:

.. code-block:: bash

   # Basic analysis
   ./build/bin/tpa input.bc

   # With external pointer table and output CFG graphs
   ./build/bin/tpa -ext ext_table.txt -cfg-dot-dir cfgs/ input.bc

   # Print points-to sets and indirect call targets
   ./build/bin/tpa -print-pts -print-indirect-calls input.bc

   # Skip prepass and save preprocessed IR
   ./build/bin/tpa -no-prepass -prepass-out preprocessed.bc input.bc

AserPTA (aser-aa)
-----------------

High-performance constraint-based pointer analysis with multiple context
sensitivities and solver algorithms.

**Binary**: ``aser-aa``  
**Location**: ``tools/alias/aser-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/aser-aa [options] input.bc

**Key Options** (see also :doc:`../../analysis/alias_analysis` and ``TOOLS.md``):

- **Analysis mode**:

  - ``-analysis-mode=ci`` – Context-insensitive (default)
  - ``-analysis-mode=1-cfa`` – 1-call-site sensitive
  - ``-analysis-mode=2-cfa`` – 2-call-site sensitive
  - ``-analysis-mode=origin`` – Origin-sensitive (thread creation)

- **Solver**:

  - ``-solver=basic`` – PartialUpdateSolver
  - ``-solver=wave`` – WavePropagation with SCC detection (default)
  - ``-solver=deep`` – DeepPropagation with cycle-aware propagation

- **Other**:

  - ``-field-sensitive[=true|false]`` – Field-sensitive memory model
  - ``-dump-stats`` – Print analysis statistics
  - ``-consgraph`` – Dump constraint graph (DOT)
  - ``-dump-pts`` – Dump points-to sets

**Example**:

.. code-block:: bash

   # 1-CFA with deep solver
   ./build/bin/aser-aa -analysis-mode=1-cfa -solver=deep input.bc

DFPA (dfpa)
-----------

Demand-refined function-pointer analysis for indirect-call resolution.

**Binary**: ``dfpa``
**Location**: ``tools/alias/dfpa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/dfpa [options] input.bc

Key options:

- ``-indirect-ctx-k=<N>`` – selective context depth on indirect edges
- ``-refine-ambiguous-only=<bool>`` – refine only unresolved indirect calls
- ``-max-offset-depth=<N>`` – bound offset-path depth
- ``-max-demand-states=<N>`` – demand refinement state budget
- ``-enable-signature-filter=<bool>`` – intersect candidates with signature matches
- ``-output-file=<path|cout>`` – dump refined targets

Call Graph Construction (call-graph)
------------------------------------

Unified call-graph construction tool that can drive several underlying pointer
or call-graph analyses.

**Binary**: ``call-graph``
**Location**: ``tools/alias/call-graph.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/call-graph -cg-type=dyck input.bc
   ./build/bin/call-graph -cg-type=dfpa -emit-cg-as-json input.bc

Key options:

- ``-cg-type=dyck|lotus|dfpa|fpa-flta|fpa-mlta|fpa-mltadf|fpa-kelp|aserpta-ci|aserpta-1cfa|aserpta-2cfa``
- ``-emit-cg-as-dot`` or ``-emit-cg-as-json``
- ``-o <file>`` – output destination
- ``-S`` – compute graph statistics

DyckAA (dyck-aa)
----------------

Unification-based alias analysis using Dyck-CFL reachability.

**Binary**: ``dyck-aa``  
**Location**: ``tools/alias/dyck-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/dyck-aa [options] input.bc

**Key Options**:

- ``-print-alias-set-info`` – Print alias sets and relations (DOT)
- ``-count-fp`` – Count possible targets for each function pointer
- ``-dot-dyck-callgraph`` – Generate call graph from alias results
- ``-no-function-type-check`` – Disable function type compatibility checking

**Typical Use**:

.. code-block:: bash

   # Dump alias sets and Dyck-based call graph
   ./build/bin/dyck-aa -print-alias-set-info -dot-dyck-callgraph input.bc

LotusAA (lotus-aa)
------------------

Lotus-specific, flow-sensitive and field-sensitive pointer analysis with
on-the-fly call graph construction.

**Binary**: ``lotus-aa``  
**Location**: ``tools/alias/lotus-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-aa [options] input.bc

**Characteristics**:

- Flow-sensitive intra-procedural analysis
- Inter-procedural summarization and iterative call graph refinement
- Designed for high precision on smaller/medium programs

FPA (Function Pointer Analysis)
-------------------------------

Function pointer analysis toolbox (FLTA, MLTA, MLTADF, KELP) for resolving
indirect calls.

**Binary**: ``fpa``  
**Location**: ``tools/alias/fpa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/fpa [options] input.bc

**Key Options**:

- ``-analysis-type=<N>``:

  - ``1`` – FLTA
  - ``2`` – MLTA
  - ``3`` – MLTADF
  - ``4`` – KELP (USENIX Security'24)

- ``-max-type-layer=<N>`` – Maximum type layer for MLTA (default: 10)
- ``-debug`` – Enable debug output
- ``-output-file=<path>`` – Output file (\"cout\" for stdout)

**Example**:

.. code-block:: bash

   # KELP-based function pointer analysis
   ./build/bin/fpa -analysis-type=4 -debug input.bc

Sea-DSA Tools
-------------

Sea-DSA-based memory graph construction and analysis.

**Binaries**:

- ``sea-dsa-dg`` – Simple Sea-DSA driver
- ``seadsa-tool`` – Advanced Sea-DSA analysis tool

**Locations**:

- ``tools/alias/sea-dsa-dg.cpp``
- ``tools/alias/seadsa-tool.cpp``

**Usage**:

.. code-block:: bash

   # Basic memory graph generation
   ./build/bin/sea-dsa-dg --sea-dsa-dot input.bc

   # Advanced analysis with DOT output directory
   ./build/bin/seadsa-tool --sea-dsa-dot --outdir=results/ input.bc

**Key Options**:

- ``--sea-dsa-dot`` – Generate DOT memory graphs
- ``--sea-dsa-callgraph-dot`` – Generate call graph DOT (if enabled)
- ``--outdir <DIR>`` – Output directory for generated artifacts

DynAA (Dynamic Alias Analysis)
------------------------------

Dynamic checker that validates static alias analyses against runtime behavior.

**Binaries**:

- ``dynaa-instrument`` – Instrument program to log pointer behavior
- ``dynaa-check`` – Compare runtime logs against static AA
- ``dynaa-log-dump`` – Dump binary logs to readable format

**Location**: ``tools/alias/dynaa/``

**Typical Workflow**:

.. code-block:: bash

   # 1. Compile to LLVM bitcode
   clang -emit-llvm -c example.cpp -o example.bc

   # 2. Instrument the bitcode
   ./build/bin/dynaa-instrument example.bc -o example.inst.bc

   # 3. Build and run the instrumented binary
   clang example.inst.bc build/libRuntime.a -o example.inst
   LOG_DIR=logs/ ./example.inst

   # 4. Check static analysis (e.g., basic-aa) against runtime log
   ./build/bin/dynaa-check example.bc logs/pts.log basic-aa

   # 5. Optionally dump the log
   ./build/bin/dynaa-log-dump logs/pts.log

**Use Cases**:

- Validate soundness/precision of a static alias analysis
- Investigate mismatches between static and dynamic behavior
- Support research on alias analysis accuracy

See also the detailed description in ``tools/alias/dynaa/README.md``.
