Pointer Analysis Metrics
========================

``include/Alias/Metrics/`` and ``lib/Alias/Metrics/`` provide helpers for
measuring precision and soundness-related properties of alias analyses.

**Main components**:

- ``PointerAnalysisMetrics`` stores collected statistics.
- ``CollectMetricsOptions`` configures metric collection.
- ``collectMetricsFromWrapper`` runs the collection against an alias-analysis
  wrapper.

This module is primarily for evaluation, comparison, and regression tracking of
points-to algorithms.

See also :doc:`alias_analysis`.
