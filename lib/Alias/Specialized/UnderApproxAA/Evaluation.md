# Evaluation Must-Alias Analysis

## Optimizations

Redundant load / store elimination  (a.k.a. perfect value forwarding): In SSA form, two memory ops load p / load q can be merged iff p and q must alias and no clobber in between

Register promotion of stack/heap objects: If every pointer to an object must alias one static allocation, promote it to a scalar in a register. 

Load hoisting out of loops: If A[i] and A[j] must alias (e.g., i==j for all iterations), you can hoist one of them. 

## Analysis/Verification

- Lock‐consistency checking: If the protected variable pointer you pass into a critical section must alias the pointer stored in the lock’s metadata, you can prove that every access is indeed locked.\

- Null pointer detection: (maybe follow the formulation of "incorrectness logic"...)


Mabye?:

- Separation logic frame conditions often require proving ∗p and ∗q are disjoint; proving the opposite (must alias) allows you to merge footprints and avoid “missing case” obligations.
- Ghost variables: if ghost_old and ghost_new must alias the same region you can omit havoc steps.