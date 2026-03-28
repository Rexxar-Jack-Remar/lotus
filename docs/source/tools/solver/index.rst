Solver Tools
============

This page documents the small command-line front-ends under ``tools/solver/``.

OWL – SMT/Model Checking Front-End
----------------------------------

``owl`` is a lightweight front-end for feeding SMT-LIB2 problems to the
configured SMT solver (Z3 in the default build).

**Binary**: ``owl``  
**Location**: ``tools/solver/owl.cpp``

**Note**: This tool is optional and must be enabled with ``BUILD_OWL=ON`` during
CMake configuration.

**Usage**:

.. code-block:: bash

   ./build/bin/owl file.smt2

**Example**:

.. code-block:: bash

   ./build/bin/owl examples/solver/example.smt2

See :doc:`../../solvers/smt` for details about the solver stack.

SLOT – SMT-LIB to LLVM Translator
---------------------------------

``slot`` translates SMT-LIB2 formulas into LLVM IR and can optionally run a
small optimization pipeline over the generated function.

**Binary**: ``slot``

**Source**: ``tools/solver/slot.cpp``

Basic usage:

.. code-block:: bash

   ./build/bin/slot -s query.smt2 -o query.ll
   ./build/bin/slot -s query.smt2 -o query.ll -pall

Important options:

- ``-s <file>`` – input SMT-LIB2 file
- ``-o <file>`` – output file
- ``-lu <file>`` / ``-lo <file>`` – dump LLVM before or after optimization
- ``-m`` – rewrite constant shifts as multiplication
- ``-pall`` – run the built-in optimization pipeline

SLOT can also run individual LLVM passes such as ``-instcombine``, ``-sccp``,
``-dce``, and ``-gvn``.

STAUB – Bounded-Theory Conversion Front-End
-------------------------------------------

``staub`` rewrites unbounded SMT constraints into bounded encodings before
translation or solving.

**Binary**: ``staub``

**Source**: ``tools/solver/staub.cpp``

Basic usage:

.. code-block:: bash

   ./build/bin/staub -s query.smt2 -i aix -o bounded.smt2
   ./build/bin/staub -s query.smt2 -r 8,24 -o bounded.smt2

Important options:

- ``-s <file>`` – input SMT-LIB2 file
- ``-o <file>`` – output transformed formula
- ``-t <file>`` – write statistics
- ``-l`` – emit output compatible with SLOT
- ``-i <N|aix|aix2>`` – integer bounding mode
- ``-r <ebits,sbits|aix|aix4>`` – floating-point bounding mode
