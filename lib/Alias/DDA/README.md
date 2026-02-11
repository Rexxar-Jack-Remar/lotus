# Demand-Driven Pointer Analysis (DDA)

Value-flow-based demand-driven pointer analysis following **SVF's FlowDDA / DDAVFSolver** design (FSE'16, TSE'18). It answers points-to and alias queries on demand by backward traversal on the Sparse Value-Flow Graph (SVFG), instead of computing whole-program points-to up front.


## References

- Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow Refinement". FSE'16.
- Yulei Sui, Jingling Xue. "Value-Flow-Based Demand-Driven Pointer Analysis for C and C++". TSE'18.
