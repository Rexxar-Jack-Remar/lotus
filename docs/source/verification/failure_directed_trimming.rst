Failure-Directed Trimming
=========================

``FailureDirectedTrimming`` contains verification support for reducing programs
or paths around observed failure behavior.

**Headers**: ``include/Verification/FailureDirectedTrimming/``

**Implementation**: ``lib/Verification/FailureDirectedTrimming/``

Overview
--------

This subsystem provides internal support for trimming verification problems so
backends can focus on failure-relevant behavior. It is currently infrastructure
code rather than a documented standalone end-user tool.

Use cases
---------

- Reduce search space around failure witnesses.
- Support verification backends that benefit from smaller failure-focused IR.
- Provide reusable transforms for experimental verification pipelines.

Notes
-----

The current documentation is intentionally high level because this subsystem is
not yet exposed through a dedicated stable front-end.
