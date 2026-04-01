Random Number Utilities
=======================

``include/Utils/Random/`` and ``lib/Utils/Random/`` provide Lotus's shared
random-number helper.

**Main component**:

- ``RNG`` wraps MT19937-style pseudo-random generation for repeatable internal
  experiments.

This utility is primarily used by testing, fuzzing support, and experimental
analysis code that needs deterministic randomization.

See also :doc:`utilities`.
