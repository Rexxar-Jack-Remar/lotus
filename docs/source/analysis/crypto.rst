Constant-Time Analysis
======================

Static analysis for verifying constant-time programming in cryptographic code.

**Headers**: ``include/Security/ConstantTime``

**Implementation**: ``lib/Security/ConstantTime``

Overview
--------

The ConstantTime module implements CT-LLVM, a static analysis tool for
verifying constant-time programming properties in sensitive code.
Constant-time programming is essential for preventing timing-based side-channel
attacks, including but not limited to cryptographic workloads.

**Related work**:

- CT-LLVM: Automatic Large-Scale Constant-Time Analysis (2025)
- ECOOP 24: CtChecker: A Precise, Sound and Efficient Static Analysis for Constant-Time Programming

Main Components
---------------

CTPass
~~~~~~

**File**: ``ctllvm.cpp``, ``ctllvm.h``

The main pass that orchestrates constant-time analysis for an entire module.

**Analysis Modes**:

1. **Soundness Mode** (``SOUNDNESS_MODE=1``):
   - Recursively inlines function calls
   - Attempts to prove constant-time properties
   - Reports "proved-CT" or "proved-NCT" for each function

2. **Standard Mode**:
   - Analyzes functions without full inlining
   - Faster but may miss some violations

**Key Features**:

- Taint tracking for secret data
- Leak detection through multiple channels:
  - Cache timing leaks
  - Branch timing leaks
  - Variable timing leaks
- Support for user-specified taint sources and declassified values
- Statistics collection for analysis coverage

Taint Analysis
~~~~~~~~~~~~~~

**File**: ``ctllvm_analysis.cpp``

Performs taint propagation and leak detection within functions.

**Leak Channels**:

1. **Cache timing**: Memory accesses that depend on secret data
2. **Branch timing**: Conditional branches that depend on secret data
3. **Variable timing**: Operations with execution time depending on secret data

**Analysis Process**:

1. **Taint Source Identification**:
   - From user specifications (target values)
   - From function arguments
   - From debug information

2. **Taint Propagation**:
   - Tracks taint through def-use chains
   - Uses alias analysis for memory operations
   - Handles control flow dependencies

3. **Leak Detection**:
   - Must-leak analysis (proven leaks)
   - May-leak analysis (potential leaks, if enabled)

4. **Reporting**:
   - Reports violations with location information
   - Classifies leak types (cache/branch/variable timing)

Inlining Support
~~~~~~~~~~~~~~~~

**File**: ``ctllvm_inlining.cpp``

Provides recursive inlining capabilities for soundness mode.

**Features**:

- Recursive function inlining
- Handles indirect calls and function pointers
- Inline threshold control
- Error handling for unsupported constructs

Configuration
-------------

The analysis supports extensive configuration through preprocessor defines:

**Analysis Modes**:

- ``SOUNDNESS_MODE``: Enable soundness mode with full inlining
- ``ENABLE_MAY_LEAK``: Enable may-leak analysis in addition to must-leak
- ``USER_SPECIFY``: Enable user-specified taint sources

**Limits and Thresholds**:

- ``ALIAS_THRESHOLD``: Maximum number of memory operations to analyze
- ``INLINE_THRESHOLD``: Maximum inlining depth/iterations

**Debugging and Statistics**:

- ``DEBUG``: Enable debug output
- ``REPORT_LEAKAGES``: Enable leak reporting
- ``TIME_ANALYSIS``: Enable timing statistics
- ``PRINT_FUNCTION``: Print IR of analyzed functions

Usage
-----

**Basic Usage**:

The CT-LLVM pass is typically invoked as part of an LLVM pass pipeline:

.. code-block:: cpp

   #include <Security/ConstantTime/ctllvm.h>
   
   ModulePassManager MPM;
   MPM.addPass(CTPass());
   MPM.run(M, MAM);

**User-Specified Taint Sources**:

When ``USER_SPECIFY=1``, users can specify which values to track as taint sources:

.. code-block:: cpp

   struct target_value_info {
     StringRef function_name;
     StringRef value_name;
     StringRef value_type;  // Optional: for struct fields
     StringRef field_name;  // Optional: for struct fields
   };

**Output**:

The analysis outputs:

- Function-level results: "proved-CT" or "proved-NCT"
- Leak reports with location information
- Statistics about analyzed functions, violations, and analysis coverage

**Typical use cases**:

- Verifying cryptographic implementations are constant-time
- Security auditing of sensitive code
- CI/CD integration for security checks
- Research on constant-time programming

Limitations
-----------

- Requires debug information for precise variable tracking
- May have false positives in may-leak mode
- Soundness mode may fail on functions that cannot be fully inlined
- Analysis complexity grows with alias threshold
