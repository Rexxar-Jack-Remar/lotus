Optimization Tools
==================

This section documents the optimization-related command-line tools under
``tools/optimization/``.

The current front ends cover two focused parts of ``lib/Optimization/``:

- ``lotus-opt-ipo`` drives the passes in ``lib/Optimization/IPO/`` via
  ``tools/optimization/lotus-opt-ipo.cpp``
- ``lotus-opt-prefetch`` drives the software prefetching implementation in
  ``lib/Optimization/Prefetch/`` via
  ``tools/optimization/lotus-opt-prefetch.cpp``

Other optimization libraries, such as ``Scalar/``, ``Pipeline/``, and
``PartialEvaluation/``, exist in the source tree but are not documented here as
standalone tools because this directory does not currently expose separate
front-end binaries for them.

**Location**: ``tools/optimization/``
**Additional frontends**: ``tools/optimization/lotus-opt-purity.cpp`` and the
currently disabled ``tools/optimization/lotus-opt-lif.cpp``

.. toctree::
   :maxdepth: 1

   interprocedural
   prefetch
