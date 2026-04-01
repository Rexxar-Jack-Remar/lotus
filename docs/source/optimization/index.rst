Optimization
============

This section covers optimization passes and utilities provided by Lotus.
These passes are useful for preparing code for analysis or for improving
performance. They complement the code transformations available in
:doc:`../transform/index`.

**Headers**: ``include/Optimization``

**Implementation**: ``lib/Optimization``

ModuleOptimizer
---------------

**File**: ``ModuleOptimizer.cpp``, ``ModuleOptimizer.h``

Provides a simple interface for applying standard LLVM optimization pipelines
(O0, O1, O2, or O3) to an entire module.

**Usage**:

.. code-block:: cpp

   #include <Optimization/ModuleOptimizer.h>

   llvm::Module *M = ...;
   
   // Apply O2 optimizations
   llvm_utils::optimiseModule(M, llvm::OptimizationLevel::O2);

**When to use**:

- Apply standard LLVM optimizations before running analyses
- Prepare code for verification or further transformation
- Test analysis tools on optimized code

AInliner (Aggressive Inliner)
------------------------------

**File**: ``AInliner.cpp``

An aggressive inliner that attempts to inline as many function calls as
possible. This pass is tuned for analysis-friendly IR where maximum inlining
may be beneficial.

**Features**:

- Inlines function calls aggressively
- Supports exclusion list via ``-ainline-noinline`` command-line option
- Operates at the module level

**Command-line options**:

.. code-block:: bash

   -ainline-noinline="func1,func2"  # Functions to exclude from inlining

**When to use**:

- Reduce inter-procedural complexity for analysis
- Improve precision of intra-procedural analyses
- Prepare code where maximum inlining is desired

**Limitations**:

- May significantly increase code size
- Use with caution on large codebases

LICM (Loop Invariant Code Motion)
----------------------------------

**File**: ``LICM.cpp``, ``LICM.h``

Loop Invariant Code Motion pass taken from LLVM 14. This pass moves loop-invariant
code out of loops to improve performance.

**Features**:

- Hoists loop-invariant instructions to loop preheader
- Sinks code to exit blocks when safe
- Promotes must-aliased memory locations to registers
- Uses alias analysis for precise memory operations

**When to use**:

- Optimize loops by moving invariant computations outside
- Evaluate alias analysis precision (LICM relies on alias analysis)
- Improve code before analysis or verification

**Note**: This is the LLVM 14 version of LICM, used internally by Lotus for
evaluating alias analysis algorithms.

SWPrefetching (Software Prefetching)
-------------------------------------

See :doc:`swprefetching` for details on the profile-guided software prefetching
pass.

MemorySSA Optimizations
-----------------------

Interprocedural optimizations that use MemorySSA instrumentation (ShadowMem):

- ``IPDeadStoreElimination``: removes stores (and some global initializers) whose
  shadow.mem def-use chains never reach a load. Traverses into callees/callers
  via shadow.mem.arg.* and shadow.mem.in/out.
- ``IPRedundantLoadElimination``: within a block, removes repeated loads when
  the MemorySSA version (TLVar) and pointer operand are identical and no memory
  side effects intervene.
- ``IPStoreToLoadForwarding``: replaces loads with the unique reaching store
  value by following MemorySSA def-use chains across calls/phis. Rewrites when
  exactly one non-conflicting value is found.
- ``IPStoreSinking``: sinks stores (and their shadow.mem.store) forward within a
  block to just before the first observed use when instructions in between are
  side-effect free.

All implemented under ``lib/Optimization`` and default to singleton regions
unless configured otherwise.

See :doc:`ip` for the dedicated page for the ``lib/Optimization/IP/`` subtree.

Integration with Analysis
-------------------------

These optimization passes can be used to prepare code for analysis:

.. code-block:: cpp

   #include <Optimization/ModuleOptimizer.h>
   #include <Optimization/SWPrefetching.h>
   
   llvm::Module &M = ...;
   
   // Apply standard optimizations
   llvm_utils::optimiseModule(&M, llvm::OptimizationLevel::O1);
   
   // Then run analyses
   // ...

**Typical pipeline**:

1. Apply optimizations (ModuleOptimizer, AInliner, etc.)
2. Run alias analysis or other static analyses
3. Perform verification or bug detection

.. toctree::
   :maxdepth: 2

   ip
   pe
   swprefetching
