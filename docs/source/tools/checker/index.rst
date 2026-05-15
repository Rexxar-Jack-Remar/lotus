Checker Tools
=============

This page summarizes the unified checker frontend under ``tools/checker/``.
For feature-oriented examples, see :doc:`../../user_guide/bug_detection`.

Unified Frontend
----------------

Lotus now builds a single checker binary:

* **Binary**: ``lotus-check``
* **Directory**: ``tools/checker/``
* **Dispatch model**: one binary with subcommands such as ``generic``,
  ``kint``, ``ae``, ``taint``, ``concur``, ``pulse``, ``fitx``, ``saber``,
  and ``symex``

The subcommand runners live in:

* ``tools/checker/lotus_check.cpp``
* ``tools/checker/lotus_kint.cpp``
* ``tools/checker/lotus_ae.cpp``
* ``tools/checker/lotus_taint.cpp``
* ``tools/checker/lotus_concur.cpp``
* ``tools/checker/lotus_pulse.cpp``
* ``tools/checker/lotus_fitx.cpp``
* ``tools/checker/lotus_saber.cpp``
* ``tools/checker/lotus_symex.cpp``

Basic Usage
-----------

.. code-block:: bash

   ./build/bin/lotus-check --help
   ./build/bin/lotus-check --list-checkers
   ./build/bin/lotus-check generic input.bc --checker=forbidden.system

Subcommand Examples
-------------------

KINT:

.. code-block:: bash

   ./build/bin/lotus-check kint input.bc --check-all=true
   ./build/bin/lotus-check kint input.bc --check-int-overflow=true

AE:

.. code-block:: bash

   ./build/bin/lotus-check ae input.bc --all
   ./build/bin/lotus-check ae input.bc --overflow --null-deref

Taint:

.. code-block:: bash

   ./build/bin/lotus-check taint input.bc \
     --aa=dyck \
     --sources=recv,getenv \
     --sinks=system,execve

Concurrency:

.. code-block:: bash

   ./build/bin/lotus-check concur input.bc --checks=race,deadlock,openmp

Pulse:

.. code-block:: bash

   ./build/bin/lotus-check pulse input.bc --json-output pulse.json

FiTx:

.. code-block:: bash

   ./build/bin/lotus-check fitx input.bc --detector=uaf

Saber:

.. code-block:: bash

   ./build/bin/lotus-check saber input.bc --all

SymEx:

.. code-block:: bash

   ./build/bin/lotus-check symex input.bc

See Also
--------

* :doc:`../../checker/index`
* :doc:`../../user_guide/bug_detection`
