Points-To Set Backends
======================

``include/Alias/PtsSet/`` contains alternative data structures for representing
points-to sets inside alias analyses.

**Backends in this tree**:

- ``DenseHashPtsSet`` for hash-based storage.
- ``ChunkedSparseBitsetPtsSet`` and ``BloomBitsetPtsSet`` for compact bitset
  variants.
- ``BDDAndersPtsSet`` for a BDD-backed representation.

Some of these implementations are experimental or selectively wired into the
current analyses, but the directory is the natural home for set-representation
experiments.

See also :doc:`alias_analysis` and :doc:`metrics`.
