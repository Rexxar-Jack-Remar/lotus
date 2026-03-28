Nisse
=====

``Nisse`` contains an LLVM new-pass-manager analysis and transform under
``Transform/Nisse``.

**Headers**: ``include/Transform/Nisse/``

**Implementation**: ``lib/Transform/Nisse/``

Overview
--------

The subsystem packages a graph-based analysis and associated pass pipeline built
around ``NisseAnalysis`` and ``NissePass``. The code also provides
``KSAnalysis`` and ``KSPass`` variants.

Main components
---------------

- ``Edge`` and ``UnionFind`` implement core graph data structures.
- ``AnalysisUtil`` provides helper functionality for analysis construction.
- ``NisseAnalysis`` exposes analysis results through the new pass manager.
- ``NissePass`` and ``KSPass`` apply the transform/analysis logic to a module.

Notes
-----

This subsystem is currently documented at a high level because the public usage
surface is smaller than the more established transformation passes in
:doc:`transforms`.
