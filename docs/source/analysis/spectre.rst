Spectre Cache Analysis
======================

Cache speculation analysis for detecting Spectre vulnerabilities related to
cache timing side-channels.

**Headers**: ``include/Checker/Security/Spectre``

**Implementation**: ``lib/Checker/Security/Spectre``

Overview
--------

The Spectre analysis module provides cache modeling and speculation analysis
to detect potential cache-based side-channel vulnerabilities. It simulates
cache behavior to identify cases where secret data may leak through cache
timing differences.

Main Components
---------------

CacheModel
~~~~~~~~~~

**File**: ``CacheModel.cpp``

Models cache behavior with configurable parameters:

- **Cache line size**: Size of each cache line (default: 16 bytes)
- **Number of cache lines**: Total number of cache lines (default: 32)
- **Number of sets**: Number of cache sets for set-associative caches
- **Associativity**: Cache lines per set

**Features**:

- Tracks which memory addresses are in cache
- Simulates cache hits and misses
- Records access patterns for analysis
- Supports both must-hit and may-miss analysis modes

CacheSpecuAnalysis
~~~~~~~~~~~~~~~~~~

**File**: ``CacheSpecuAnalysis.cpp``

Performs speculative cache analysis to detect potential Spectre vulnerabilities.

**Analysis Flow**:

1. **Initialization**: Builds cache model from function arguments and global variables
2. **Cache Simulation**: Simulates cache behavior for memory accesses
3. **Speculative Analysis**: Analyzes speculative execution paths
4. **Leak Detection**: Identifies potential cache timing leaks

**Key Methods**:

- ``InitModel()`` – Initializes cache model with function parameters and globals
- ``SpecuSim()`` – Simulates speculative cache behavior between basic blocks
- ``IsValueInCache()`` – Checks if a value is currently in cache
- ``visitLoadInst()`` – Processes load instructions and updates cache state

**Configuration**:

The analysis supports several configuration options:

- ``MissSpecuDepth``: Maximum depth for speculative miss analysis
- ``MergeOption``: Option for merging cache states
- Cache parameters: line size, number of lines, number of sets

Usage
-----

The Spectre analysis is typically used as part of security analysis pipelines
to detect cache-based side-channel vulnerabilities.

**Typical use cases**:

- Detecting Spectre-variant vulnerabilities
- Analyzing cache timing side-channels
- Security auditing of cryptographic code
- Verifying constant-time implementations

**Dependencies**:

- Dominator Tree analysis
- Post-Dominator Tree analysis
- Alias Analysis

**Limitations**:

- Analysis is intra-procedural (function-level)
- Requires precise alias analysis for accurate results
- Cache model is simplified compared to real hardware
