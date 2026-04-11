Optimization Tools
==================

This section documents the optimization-related command-line tools under
``tools/optimization/``.

The current front ends cover two focused parts of ``lib/Optimization/``:

- ``lotus-ipo`` drives the passes in ``lib/Optimization/IPO/``
- ``lotus-prefetch`` drives the software prefetching implementation in
  ``lib/Optimization/Prefetch/``

Other optimization libraries, such as ``Scalar/``, ``Pipeline/``, and
``PartialEvaluation/``, exist in the source tree but are not documented here as
standalone tools because this directory does not currently expose separate
front-end binaries for them.

**Location**: ``tools/optimization/``

.. toctree::
   :maxdepth: 1

   interprocedural
   prefetch
