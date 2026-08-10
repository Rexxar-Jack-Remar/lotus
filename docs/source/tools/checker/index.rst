Checker Tools
=============

This page summarizes the unified checker frontend under ``tools/checker/``.
For feature-oriented examples, see :doc:`../../user_guide/bug_detection`.

Unified Frontend
----------------

Lotus now builds a single checker binary:

* **Binary**: ``lotus-check``
* **Directory**: ``tools/checker/``
* **Dispatch model**: one binary with explicit engine values such as ``generic``,
  ``kint``, ``ae``, ``taint``, ``concur``, ``pulse``, ``fitx``, ``saber``,
  and ``symex``

The engine runners live in:

* ``tools/checker/lotus-check-generic.cpp``
* ``tools/checker/lotus-check-kint.cpp``
* ``tools/checker/lotus-check-ae.cpp``
* ``tools/checker/lotus-check-taint.cpp``
* ``tools/checker/lotus-check-concur.cpp``
* ``tools/checker/lotus-check-pulse.cpp``
* ``tools/checker/lotus-check-fitx.cpp``
* ``tools/checker/lotus-check-saber.cpp``
* ``tools/checker/lotus-check-symex.cpp``

Basic Usage
-----------

.. code-block:: bash

   ./build/bin/lotus-check --help
   ./build/bin/lotus-check --list-checkers
   ./build/bin/lotus-check --engine=generic input.bc --checker=forbidden.system

``--list-checkers`` lists both generic checker ids and native engines, with a
``MODE`` column showing which entries are accepted by ``--checker`` and which
must be selected through ``--engine``. For a bug-class-to-engine guide, see
:ref:`Choosing a Checker <choosing-a-checker>`.

Engine Examples
---------------

KINT:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --check-all=true
   ./build/bin/lotus-check --engine=kint input.bc --check-int-overflow=true

AE:

.. code-block:: bash

   ./build/bin/lotus-check --engine=ae input.bc --all
   ./build/bin/lotus-check --engine=ae input.bc --overflow --null-deref

Taint:

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint input.bc \
     --aa=dyck \
     --sources=recv,getenv \
     --sinks=system,execve

Concurrency:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc --checks=race,deadlock,openmp

Pulse:

.. code-block:: bash

   ./build/bin/lotus-check --engine=pulse input.bc --report-json=pulse.json

FiTx:

.. code-block:: bash

   ./build/bin/lotus-check --engine=fitx input.bc

Saber:

.. code-block:: bash

   ./build/bin/lotus-check --engine=saber input.bc --all

SymEx:

.. code-block:: bash

   ./build/bin/lotus-check --engine=symex input.bc

The ``symex`` engine runner is implemented in ``tools/checker/lotus-check-symex.cpp``
and links the top-level ``CanarySymbolicExecution`` library from
``lib/SymbolicExecution``.

See Also
--------

* :doc:`../../checker/index`
* :doc:`../../user_guide/bug_detection`
