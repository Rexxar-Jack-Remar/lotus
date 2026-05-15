Checker Framework
==================

The Checker Framework provides a unified infrastructure for static bug detection across multiple vulnerability categories. It integrates various analysis techniques to detect security vulnerabilities, concurrency bugs, and numerical errors in LLVM bitcode.

**Location**: ``lib/Checker/``

**Headers**: ``include/Checker/``

**Tool Frontend**: ``lotus-check`` with subcommand runners in ``tools/checker/``

Overview
--------

The Checker Framework consists of several checker categories, all unified through a centralized bug reporting system:

* **AE Checkers** – Abstract-execution-based memory-safety bug detection
* **FiTx Checkers** – Daily development-friendly bug detection using typestate analysis (path-insensitive, return-code aware; see Suzuki et al., USENIX ATC 2024)
* **KINT Checkers** – Numerical bugs (overflow, division by zero, array bounds) using range analysis and SMT solving
* **Concurrency Checkers** – Thread safety and parallel-runtime issues (data races, deadlocks, atomicity violations, OpenMP bugs, MPI bugs) using MHP, lock set, OpenMP, and MPI analyses
* **Pulse Checker** – Memory safety and other bugs using biabductive analysis with path-sensitive interprocedural reasoning
* **Saber Checkers** – Source-sink bug detection over sparse value-flow graphs

All checkers report bugs through the centralized ``BugReportMgr`` system, enabling unified output formats (JSON, SARIF) and consistent bug reporting across all analysis tools. The repository now builds a single checker binary, ``lotus-check``, with subcommands such as ``kint``, ``ae``, ``pulse``, ``saber``, and ``concur``.

Components
----------

**Concurrency Checkers** (``lib/Checker/Concurrency/``):

* ``ConcurrencyChecker.cpp`` – Main concurrency checker coordinator
* ``DataRaceChecker.cpp`` – Data race detection using MHP (May Happen in Parallel) analysis
* ``DeadlockChecker.cpp`` – Deadlock detection using lock set analysis
* ``AtomicityChecker.cpp`` – Atomicity violation detection
* ``OpenMPChecker.cpp`` – Dedicated OpenMP bug checks built on OpenMP task analysis
* ``MPIChecker.cpp`` – Dedicated MPI bug checks built on MPI communication/RMA analysis

**AE Checkers** (``lib/Checker/AE/``):

* ``AbstractInterpretation.cpp`` – Fixpoint engine for abstract execution
* ``AEDetector.cpp`` – Bug-specific detector integration
* ``AbstractState.*`` / ``AbstractValue.*`` – State and value abstractions

**FiTx Bug Checkers** (``lib/Checker/FiTx/``):

* ``frontend/Framework.cpp`` – Main FiTx pass; typestate-based daily development-friendly checkers (Suzuki et al., USENIX ATC 2024)
* ``frontend/Analyzer.cpp`` – CFG-based typestate analysis with return-code aware state propagation
* ``framework_ir/Analyzer.cpp`` – IR builder; collects return values for function summaries
* ``detector/df_detector/``, ``uaf_detector/``, ``leak_detector/``, etc. – Typestate definitions per bug pattern

**KINT Numerical Checkers** (``lib/Checker/KINT/``):

* ``MKintPass.cpp`` – Main KINT pass for integer overflow, division by zero, array bounds checking
* ``MKintPass_bugreport.cpp`` – Bug report generation for KINT
* ``RangeAnalysis.cpp`` – Range analysis for variables using abstract interpretation
* ``KINTTaintAnalysis.cpp`` – Taint analysis integration for tracking untrusted data
* ``BugDetection.cpp`` – Bug detection and reporting logic
* ``Options.cpp`` – Command-line option parsing
* ``Log.cpp`` – Logging utilities
* ``Utils.cpp`` – Utility functions

**Pulse Checker** (``lib/Checker/Pulse/``):

* ``PulseChecker.cpp`` – Main bug finder using biabductive analysis
* ``PulseDomain.cpp`` – Execution domain abstraction
* ``PulseAbductiveDomain.h`` – Core abstract domain with biabduction
* ``PulseOperations.cpp`` – Core memory operations (readDeref, writeDeref, etc.)
* ``PulseDisjunctiveDomain.cpp`` – Disjunctive analysis for path-sensitive reasoning
* ``PulseLoopAbstraction.cpp`` – Loop abstraction with widening
* ``PulseSummary.cpp`` – Function summary representation and application
* ``PulseTaint.cpp`` – Taint analysis for security vulnerabilities
* ``PulseModels.cpp`` – Library function models
* ``PulseDiagnostic.cpp`` – Rich diagnostic reporting with traces

**Saber Checker** (``lib/Checker/Saber/``):

* ``LeakChecker.cpp`` – Leak detection over source-sink flows
* ``DoubleFreeChecker.cpp`` – Double-free detection
* ``FileChecker.cpp`` – File-descriptor leak checking
* ``SaberCheckerAPI.cpp`` – Reusable checker API surface

**Report System** (``lib/Checker/Report/``):

* ``BugReport.cpp`` – Bug report data structures with source location information
* ``BugReportMgr.cpp`` – Centralized bug report management (singleton pattern)
* ``BugTypes.cpp`` – Bug type definitions, classifications, and CWE mappings
* ``SARIF.cpp`` – SARIF format output support
* ``ReportOptions.cpp`` – Report configuration options (JSON, SARIF output)

**Debug Info Analysis** (``lib/Analysis/DebugInfo/``):

* ``DebugInfoAnalysis.cpp`` – Debug information extraction from LLVM metadata

Build Targets
-------------

* ``FiTxChecker`` – FiTx typestate-based bug checker library
* ``PulseChecker`` – Pulse biabductive analysis checker library
* ``lotus-check`` – Unified checker frontend
* ``tools/checker/lotus-check-ae.cpp`` – AE subcommand runner
* ``tools/checker/lotus-check-fitx.cpp`` – FiTx subcommand runner
* ``tools/checker/lotus-check-kint.cpp`` – KINT subcommand runner
* ``tools/checker/lotus-check-concur.cpp`` – Concurrency subcommand runner
* ``tools/checker/lotus-check-pulse.cpp`` – Pulse subcommand runner
* ``tools/checker/lotus-check-saber.cpp`` – Saber subcommand runner
* ``tools/checker/lotus-check-symex.cpp`` – Symbolic-execution subcommand runner
* ``tools/checker/lotus-check-taint.cpp`` – Taint-analysis subcommand runner

Usage
-----

**KINT Tool**:

.. code-block:: bash

   ./build/bin/lotus-check kint input.bc --check-all=true
   ./build/bin/lotus-check kint input.bc --check-int-overflow=true --check-div-by-zero=true
   ./build/bin/lotus-check kint input.bc --report-json=report.json

**Concurrency Tool**:

.. code-block:: bash

   ./build/bin/lotus-check concur input.bc --check-data-races
   ./build/bin/lotus-check concur input.bc --check-deadlocks --check-atomicity
   ./build/bin/lotus-check concur input.bc --checks=openmp,mpi
   ./build/bin/lotus-check concur input.bc --report-json=report.json

**Pulse Tool**:

.. code-block:: bash

   ./build/bin/lotus-check pulse input.bc
   ./build/bin/lotus-check pulse input.bc -v
   ./build/bin/lotus-check pulse input.bc --log-level=debug

Programmatic Usage
------------------

All checkers integrate with the centralized ``BugReportMgr``:

.. code-block:: cpp

   #include "Checker/Report/BugReportMgr.h"
   
   // Access centralized reports emitted by checker frontends
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, 0);

Bug Types
---------

The framework detects the following bug categories:

* **Memory Safety**: Null pointer dereference, use-after-free, uninitialized reads, invalid accesses, memory leaks
* **Numerical Errors**: Integer overflow, division by zero, bad shift, array out-of-bounds, dead branches
* **Concurrency**: Data races, deadlocks, atomicity violations, lock mismatches, condition-variable misuse, OpenMP runtime misuse, MPI protocol/RMA bugs
* **Security**: Taint errors (untrusted data flows), use-after-free, null dereferences
* **Performance**: Unnecessary copies, const-refable parameters

All bug types are classified by importance (LOW, MEDIUM, HIGH) and category (SECURITY, ERROR, WARNING, PERFORMANCE) with CWE mappings.

Integration Points
------------------

* **UnderApproxAA**: Used by PulseChecker for must-alias canonicalization
* **Z3 SMT Solver**: Used by KINT for path-sensitive verification
* **Biabductive Analysis**: Used by PulseChecker for precise bug detection
* **LLVM Pass Infrastructure**: Standard pass registration for integration

See Also
--------

.. toctree::
   :maxdepth: 1

   ae
   concurrency
   fitx
   kint
   pulse
   report
   saber
