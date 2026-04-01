Core Utility Types
==================

``include/Utils/Types/`` contains small compatibility and convenience headers
used throughout the codebase.

**Main components**:

- ``Optional`` for C++14-friendly optional values.
- ``Nullable`` and ``Offset`` for common analysis-side data wrappers.
- ``ScopeExit`` for lightweight RAII cleanup.
- ``range.h`` and vendored terminal-color helpers for ergonomic utility code.

These headers are intentionally low-level and widely reused.

See also :doc:`utilities`.
