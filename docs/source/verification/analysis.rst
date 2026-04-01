Verification Support Analyses
=============================

``include/Verification/Analysis/`` and ``lib/Verification/Analysis/`` collect
small helper passes used by verification-oriented pipelines.

**Main components**:

- ``ClassifyInstructionsPass`` and ``CountInstrPass`` inspect module structure.
- ``ClassifyLoopsPass`` and ``GetTestTargetsPass`` identify verification-relevant
  regions.
- ``CheckModulePass`` performs lightweight module validation.

These passes are mainly support utilities for verifier front-ends rather than
standalone end-user analyses.

See also :doc:`backend` and :doc:`transforms`.
