Checker Report Infrastructure
=============================

``include/Checker/Report/`` and ``lib/Checker/Report/`` implement the shared
bug-reporting layer used by all Lotus checkers.

**Main components**:

- ``BugReport`` and ``BugDiagStep`` store findings and diagnostic traces.
- ``BugReportMgr`` is the central aggregation and emission entry point.
- ``BugTypes`` defines checker-visible bug categories and metadata.
- ``SuppressionManager`` filters findings by suppression rules.
- ``SARIF`` support emits standardized machine-readable reports.

Every major checker family routes its output through this layer so tools can
share JSON, SARIF, and summary reporting.

See also :doc:`index` and :doc:`../user_guide/bug_detection`.
