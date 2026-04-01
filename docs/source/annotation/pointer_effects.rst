Pointer Effect Specifications
=============================

The ``include/Annotation/Pointer/`` headers define summary objects for pointer
allocation, copy, and exit effects of external APIs.

**Location**: ``include/Annotation/Pointer/``

**Main APIs**:

- ``PointerEffect`` is the tagged union for allocation, copy, and exit effects.
- ``PointerEffectSummary`` groups effects for one function.
- ``ExternalPointerTable`` stores summaries keyed by function name.
- ``ExternalPointerTablePrinter`` prints loaded summaries.

**What it models**:

- Allocation sites and optional size arguments.
- Copy flows between values, direct memory, reachable memory, and special
  sources such as null or universal.
- Functions that terminate the process.

These summaries are the main bridge between external API specifications and the
pointer-analysis pipeline.

See also :doc:`annotation`, :doc:`../alias/spec`, and :doc:`../alias/allocaa`.
