PE
==

``Optimization/PE`` contains the LLPE partial-evaluation infrastructure.

**Headers**: ``include/Optimization/PE/``

**Implementation**: ``lib/Optimization/PE/``

Overview
--------

This subsystem contains the historical LLPE code used for aggressive
specialization, symbolic execution style reasoning, and partial evaluation over
LLVM IR. It is a large internal subsystem and is not currently surfaced through
the main ``lotus-opt`` front-end.

Main components
---------------

- ``LLPE.h`` defines the main analysis and integration machinery.
- ``ShadowInlines.h`` models shadow values, instructions, stores, and
  specialized state used during specialization.
- ``SharedTree.h`` provides persistent tree structures used to represent shared
  analysis state.
- ``LLPECopyPaste.h`` contains cloning and IR-rewriting helpers.

Notes
-----

The PE subsystem is primarily infrastructure code. If you are looking for
front-end optimization passes exposed as stable tools, start with
:doc:`index` and :doc:`swprefetching`.
