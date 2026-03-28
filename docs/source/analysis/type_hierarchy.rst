Type Hierarchy
==============

``TypeHirarchy`` provides class-hierarchy recovery for C++-style programs.

**Headers**: ``include/Analysis/TypeHirarchy/``

**Implementation**: ``lib/Analysis/TypeHirarchy/``

Overview
--------

This subsystem reconstructs inheritance and virtual-function-table structure
from LLVM IR and debug information. It is useful for analyses that need dynamic
type information or devirtualization-style reasoning.

Main components
---------------

- ``TypeHierarchy`` defines the generic hierarchy-query interface.
- ``DIBasedTypeHierarchy`` and ``DIBasedTypeHierarchyData`` recover hierarchy
  information from debug metadata.
- ``LLVMVFTable`` and ``LLVMVFTableData`` model virtual-function tables.
- ``TypeHierarchyAnalysis`` packages the functionality as an analysis pass.

Typical use cases
-----------------

- Build class-hierarchy facts for indirect-call resolution.
- Recover subtype relationships for object-oriented analyses.
- Provide a basis for vtable- and dynamic-dispatch reasoning.

Notes
-----

The directory name in the current tree is spelled ``TypeHirarchy`` to match the
existing code layout.
