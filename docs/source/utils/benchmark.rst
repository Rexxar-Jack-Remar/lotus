Microbenchmark Helpers
======================

``include/Utils/Benchmark/`` contains lightweight benchmarking helpers used to
measure internal analysis performance.

**Main components**:

- ``Microbench.h`` provides templated benchmark wrappers.
- ``ccutils::Stats`` accumulates timing summaries and simple statistics.

This module is header-only and is mainly intended for local experiments,
regression measurement, and algorithm comparisons.

See also :doc:`utilities`.
