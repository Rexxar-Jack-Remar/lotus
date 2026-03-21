Pulse Checker
==============

PulseChecker is a bug finder using biabductive analysis, inspired by Infer's Pulse. It performs path-sensitive interprocedural analysis to detect memory safety vulnerabilities and other bugs in LLVM bitcode.

**Library Location**: ``lib/Checker/Pulse/``

**Headers**: ``include/Checker/Pulse/``

**Tool Location**: ``tools/checker/lotus_pulse.cpp``

**Build Target**: ``lotus-pulse``

Overview
--------

PulseChecker uses biabductive analysis (combining abduction and abstract interpretation) to detect bugs by:

* **Abduction**: Inferring preconditions that would make a bug manifest
* **Abstract Interpretation**: Tracking abstract states (heap, stack, path conditions) through the program
* **Path-Sensitive Analysis**: Maintaining separate states for different execution paths
* **Interprocedural Analysis**: Using function summaries for scalable interprocedural reasoning

The analysis supports CFG-based traversal, GEP/PHI handling, UnderApproxAA for must-alias canonicalization, and interprocedural calls with function summaries.

Components
----------

**PulseChecker** (``PulseChecker.cpp``, ``PulseChecker.h``):

* Main bug finder coordinator
* Manages worklist-based CFG traversal
* Handles instruction execution and state transitions
* Coordinates interprocedural analysis via summaries
* Reports bugs through centralized ``BugReportMgr`` and ``DiagnosticManager``

**Core Domain Components**:

* ``PulseDomain.cpp`` – Execution domain abstraction (normal, abort, latent states)
* ``PulseAbductiveDomain.h`` – Core abstract domain with post-state, pre-state, and path conditions
* ``PulseMemory.cpp`` – Heap and stack abstractions
* ``PulseOperations.cpp`` – Core operations (readDeref, writeDeref, eval, etc.)
* ``PulseFormula.cpp`` – Path condition tracking using formulas

**Analysis Components**:

* ``PulseDisjunctiveDomain.cpp`` – Disjunctive analysis for path-sensitive reasoning
* ``PulseNonDisjunctiveDomain.cpp`` – Non-disjunctive domain for simpler cases
* ``PulseLoopAbstraction.cpp`` – Loop abstraction with widening and invariant inference
* ``PulseSummary.cpp`` – Function summary representation
* ``PulseSummaryApplication.cpp`` – Summary application at call sites
* ``PulseTransitiveInfo.cpp`` – Transitive information tracking for summaries

**Bug Detection Components**:

* ``PulseDiagnostic.cpp`` – Rich diagnostic reporting with traces
* ``PulseReport.cpp`` – Bug report generation
* ``PulseLatentIssue.cpp`` – Latent issue tracking (bugs that may manifest)
* ``PulseTaint.cpp`` – Taint analysis for security vulnerabilities
* ``PulseModels.cpp`` – Library function models (malloc, free, etc.)

**Supporting Components**:

* ``PulseJoin.cpp`` – State joining operations
* ``PulseSubstitution.cpp`` – Variable substitution for summaries
* ``PulseSpecialization.cpp`` – Summary specialization
* ``PulseContradiction.cpp`` – Contradiction detection
* ``PulseValueHistory.cpp`` – Value history tracking for traces
* ``PulseInvalidation.cpp`` – Memory invalidation tracking
* ``PulseCallState.cpp`` – Call state management
* ``PulseLogger.cpp`` – Logging and statistics

Bug Types Detected
------------------

**Use After Free** (``UseAfterFree``):

* **CWE**: CWE-416
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Accessing memory after it has been freed
* **Detection**: Tracks memory allocations and deallocations, detects accesses to freed memory

**Null Pointer Dereference** (``NullDereference``):

* **CWE**: CWE-476
* **Importance**: HIGH
* **Classification**: ERROR
* **Description**: Dereferencing a pointer that may be NULL
* **Detection**: Tracks pointer values, detects NULL dereferences

**Uninitialized Read** (``UninitializedRead``):

* **CWE**: CWE-457
* **Importance**: MEDIUM
* **Classification**: ERROR
* **Description**: Reading from uninitialized memory
* **Detection**: Tracks initialization state of variables, detects reads before initialization

**Unnecessary Copy** (``UnnecessaryCopy``):

* **Importance**: LOW
* **Classification**: PERFORMANCE
* **Description**: Unnecessary value copies that could be references
* **Detection**: Analyzes copy operations and suggests const-reference parameters

**Const-Refable Parameter**:

* **Importance**: LOW
* **Classification**: PERFORMANCE
* **Description**: Function parameters that could be const references instead of copies
* **Detection**: Analyzes parameter usage patterns

**Taint Error** (``TaintError``):

* **CWE**: CWE-20
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Untrusted data flowing to security-sensitive sinks
* **Detection**: Taint analysis tracks untrusted sources and detects flows to sinks

Analysis Process
----------------

1. **Module Analysis**: Iterates through all functions in the module

2. **Function Analysis**:
   
   * Initializes function entry state with parameter abstractions
   * Sets up loop abstraction and disjunctive domain
   * Performs worklist-based CFG traversal

3. **Worklist Traversal**:
   
   * Processes basic blocks in worklist order
   * Applies loop widening at loop headers
   * Joins disjunctive states when needed
   * Executes instructions and updates abstract state
   * Forks state at conditional branches
   * Tracks path conditions for path-sensitive analysis

4. **Instruction Execution**:
   
   * **Load**: Checks for null dereference, use-after-free, uninitialized read
   * **Store**: Updates heap/stack state, checks for invalid writes
   * **Call**: Handles library calls via models, interprocedural calls via summaries
   * **Alloca**: Allocates stack memory
   * **Return**: Collects exit states for summary creation
   * **PHI**: Merges values from multiple predecessors
   * **GEP**: Computes pointer arithmetic

5. **Interprocedural Analysis**:
   
   * Creates summaries from function exit states
   * Applies summaries at call sites
   * Uses materialization for precise summary application
   * Tracks transitive information across calls

6. **Bug Reporting**:
   
   * Reports manifest bugs immediately
   * Tracks latent issues (bugs that may manifest)
   * Generates rich diagnostics with execution traces
   * Reports through centralized ``BugReportMgr``

Key Features
------------

**Biabductive Analysis**:

* Infers preconditions (abduction) while tracking abstract states (abstract interpretation)
* Enables precise bug detection with minimal false positives

**Path-Sensitive Analysis**:

* Maintains separate states for different execution paths
* Uses disjunctive domains to track multiple paths
* Applies path conditions for precise reasoning

**Loop Abstraction**:

* Uses widening to ensure termination
* Infers loop invariants when possible
* Handles nested loops and complex control flow

**Function Summaries**:

* Creates summaries from function exit states
* Applies summaries at call sites for scalability
* Supports materialization for precise interprocedural analysis

**Library Models**:

* Models common library functions (malloc, free, realloc, etc.)
* Handles memory management operations precisely
* Supports custom model registration

**Latent Issues**:

* Tracks bugs that may manifest under certain conditions
* Distinguishes between manifest and latent bugs
* Provides richer diagnostic information

Usage
-----

**Basic Usage**:

.. code-block:: bash

   ./build/bin/lotus-pulse input.bc

**Verbose Output**:

.. code-block:: bash

   ./build/bin/lotus-pulse -v input.bc

**Custom Log Level**:

.. code-block:: bash

   ./build/bin/lotus-pulse --log-level=debug input.bc
   ./build/bin/lotus-pulse --log-level=trace input.bc

**Disable Statistics**:

.. code-block:: bash

   ./build/bin/lotus-pulse --pulse-stats=false input.bc

Command-Line Options
--------------------

* ``<input bitcode>`` – Required positional argument: path to LLVM bitcode file
* ``-v`` – Verbose output (enables debug logging)
* ``--log-level=<level>`` – Set log level: ``none``, ``error``, ``warning``, ``info``, ``debug``, ``trace`` (default: ``info``)
* ``--pulse-stats`` – Show Pulse analysis statistics (default: true)

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Checker/Pulse/PulseChecker.h"
   #include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
   #include "Checker/Report/BugReportMgr.h"
   
   // Setup alias analysis (UnderApproxAA recommended)
   auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
       *M, lotus::AAConfig::UnderApprox());
   
   // Create checker
   pulse::PulseChecker checker(M.get(), AA.get());
   
   // Run analysis (automatically reports to BugReportMgr)
   checker.analyze();
   
   // Access centralized reports
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, 0);

Configuration
-------------

**Analysis Limits**:

* ``kMaxDisjuncts = 10`` – Maximum number of disjunctive states per function
* ``kMaxCallDepth = 5`` – Maximum call depth for interprocedural analysis

**Alias Analysis**:

* Uses ``UnderApproxAA`` for must-alias canonicalization
* Improves precision by identifying must-alias relationships
* Optional: can run without alias analysis (less precise)

Statistics
----------

PulseChecker provides comprehensive statistics via ``PulseLogger``:

* ``modules.analyzed`` – Number of modules analyzed
* ``functions.analyzed`` – Number of functions analyzed
* ``paths.explored`` – Number of execution paths explored
* ``bugs.total`` – Total number of bugs detected
* Function-level timing information
* Path exploration statistics

Limitations
-----------

* **Scalability**: Analysis complexity grows with program size and path complexity
* **Disjunct Limit**: Limited to ``kMaxDisjuncts`` disjunctive states per function
* **Call Depth**: Limited to ``kMaxCallDepth`` for interprocedural analysis
* **Loop Handling**: May lose precision in complex loops despite widening
* **Library Modeling**: Limited to modeled library functions
* **False Positives**: May report false positives due to abstraction
* **False Negatives**: May miss bugs due to abstraction or limits

Performance
-----------

* Analysis time depends on:
  
  * Number of functions and basic blocks
  * Path complexity (number of branches)
  * Loop complexity
  * Interprocedural call depth
  
* Disjunctive analysis increases precision but also cost
* Summary-based interprocedural analysis improves scalability
* Loop widening ensures termination but may lose precision

Integration
-----------

PulseChecker integrates with:

* **AliasAnalysisWrapper**: Must-alias information for canonicalization
* **BugReportMgr**: Centralized bug reporting system
* **DiagnosticManager**: Rich diagnostic reporting with traces
* **LLVM IR**: Works directly on LLVM bitcode
* **PulseModels**: Library function modeling

Architecture
------------

**Abstract Domain**:

The analysis uses an abductive abstract domain that tracks:

* **Post-State**: Current abstract state (heap, stack, attributes)
* **Pre-State**: Inferred preconditions (biabduction)
* **Path Conditions**: Constraints via ``PulseFormula``
* **Transitive Information**: Information propagated across calls

**State Representation**:

* **Heap**: Maps abstract addresses to values and attributes
* **Stack**: Maps LLVM values to abstract addresses
* **Attributes**: Tracks properties (uninitialized, invalidated, etc.)
* **Formulas**: Path conditions and constraints

**Execution Domain**:

States are represented as ``ExecutionDomain``:

* **Normal**: Continuing execution
* **AbortProgram**: Bug detected, execution stops
* **LatentAbortProgram**: Potential bug detected
* **ExitProgram**: Function exit reached
* **ContinueProgram**: Special continuation state

See Also
--------

- :doc:`index` – Checker Framework overview
- :doc:`../alias/index` – Alias analysis for pointer information
