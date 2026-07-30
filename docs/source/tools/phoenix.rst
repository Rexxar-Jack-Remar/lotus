Phoenix — Batch Analysis Runner
================================

Phoenix is a Python-based batch analysis runner that automates running
multiple alias analysis tools (Phoenix, SVF, TPA, AserPTA, SparrowAA) over
collections of LLVM bitcode files.

**Location**: ``scripts/phoenix/``

Overview
--------

Phoenix provides an interactive command-line interface that:

1. Scans a directory for ``.bc`` files.
2. Lets the user select an analysis tool (Phoenix/SVF/TPA/Aser/Sparrow).
3. Runs the selected tool on each bitcode file.
4. Collects and writes results to structured output files.

Available Analyzers
-------------------

- ``phoenix_analyzer.py`` — Phoenix alias analysis driver
- ``svf_analyzer.py`` — SVF analysis driver
- ``tpa_analyzer.py`` — TPA (Type-based Pointer Analysis) driver
- ``aser_analyzer.py`` — AserPTA analysis driver
- ``sparrow_analyzer.py`` — SparrowAA analysis driver

Usage
-----

.. code-block:: bash

   cd lotus
   python scripts/phoenix/src/main.py

The script will prompt for a directory path containing ``.bc`` files, then ask
which tool to run.

Operational notes
-----------------

Use a directory containing bitcode built with a compatible LLVM version and
ensure the selected analyzer is available in the expected build location.
Phoenix is a batch runner: it records each input's outcome, but does not make
results from different analyzers directly comparable unless they were run with
the same bitcode, library model, and options.  Keep generated output outside
the input corpus when repeatability matters.
