Verification Backend API
========================

``include/Verification/Backend/`` and ``lib/Verification/Backend/`` define the
shared abstraction used to invoke different verification engines through one API.

**Main components**:

- ``PropertyClass`` and ``VerificationTask`` describe the requested job.
- ``VerificationResultInfo`` stores standardized results.
- ``IBackend`` is the backend interface.
- ``BackendRegistry`` manages available implementations.

The built-in registry covers SeaHorn, Sifa, SymAbsAI, and Clam.

See also :doc:`index` and :doc:`../user_guide/verification_backends`.
