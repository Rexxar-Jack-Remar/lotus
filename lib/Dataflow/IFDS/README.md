# IFDS/IDE Dataflow Analysis

## Directory Layout

### include/Dataflow/IFDS/

- **IFDSFramework.h** – Core framework (path edges, summary edges, problem interfaces).
- **Solvers/** – Header-only solver implementations:
  - **IFDSSolver.h** / **IFDSSolver.tpp** – Sequential IFDS tabulation solver.
  - **IDESolver.h** / **IDESolver.tpp** – IDE solver.
- **Clients/** – Analysis problem definitions (IFDSTaintAnalysis, IDEConstantPropagation, etc.).

### lib/Dataflow/IFDS/

- **Debug/** – Framework debug helpers (e.g. IFDSDebugUtils).
- **Clients/** – Concrete analyses built on the framework.

## Writing An Analysis

*Use IFDS**, if
- your analysis is a plain reachability problem, that is, a data-flow fact can either hold, or not.
- your analysis problem is distributive, that is, within a flow function the reachability of a successor fact may depend on at most one incoming fact.
- your analysis problem is a may-analysis, since IFDS always uses set-union as merge operator

All bit-vector problems fall into this category, including all classical gen-kill problems. Examples are taint analysis, uninitialized-variables analysis, constness analysis, etc.


*Use IDE**, if

- your analysis computes environments that associate the holding data-flow facts with an additional computed value
- OR the set of holding data-flow facts is structured, i.e. some facts subsume other facts
- OR your analysis is a must-analysis, since in contrast to IFDS the merge operator is customizable
- your analysis problem is distributive, that is, within a flow function the reachability a of a successor fact as well as its associated value may depend on at most one incoming fact/value.

Examples are linear constant-propagation, typestate analysis, type analysis, feature-taint analysis, etc.

## Related Work


You may also refer to https://github.com/secure-software-engineering/phasar/wiki/Useful-Literature

- ISSTA 23: Reducing the Memory Footprint of
IFDS-Based Data-Flow Analyses Using Fine-Grained Garbage Collection. Dongjie He, Yujiang Gui, Yaoqing Gao, and Jingling Xue.
- ICSE 21: Sustainable Solving: Reducing The Memory Footprint of IFDS-Based Data
Flow Analyses Using Intelligent Garbage Collection. Steven Arzt.
- ASE 20: Performance-Boosting Sparsification of the IFDS Algorithm with
Applications to Taint Analysis. Dongjie He, Haofeng Li, Lei Wang, Haining Meng, Hengjie Zheng, Jie Liu, Shuangwei Hu, Lian Li, and Jingling Xue.
- TACAS 19: PhASAR: An Inter-procedural Static Analysis Framework for C/C++. Philipp Dominik Schubert, Ben Hermann, and Eric Bodden.
- ECOOP 16: Boomerang: DemandDriven Flow- and Context-Sensitive Pointer Analysis for Java. Johannes Späth, Lisa Nguyen Quang Do, Karim Ali, and Eric Bodden.
- ICSE 16: StubDroid: Automatic Inference of Precise Data-Flow Summaries for the Android Framework. Steven Arzt and Eric Bodden.
- ICSE 15: Database-Backed Program Analysis for Scalable Error Propagation. Cathrin Weiss, Cindy Rubio-González, and Ben Liblit.
- ICSE 14: Reviser: Efficiently Updating IDE-/IFDS-Based Data-Flow Analyses in Response to Incremental Program Changes. Steven Arzt and Eric Bodden.
- PLDI 14: FlowDroid: Precise Context,
Flow, Field, Object-Sensitive and Lifecycle-Aware Taint Analysis for Android App. Steven Arzt, Siegfried Rasthofer, Christian Fritz, Eric Bodden, Alexandre Bartel, Jacques
Klein, Yves Le Traon, Damien Octeau, and Patrick D. McDaniel.
- CC 10: Practical Extensions to the IFDS Algorithm. Nomair A Naeem, Ondřej Lhoták, and Jonathan Rodriguez.
- CC 08: IDE Dataflow Analysis in the Presence of Large Object-Oriented Libraries. Atanas Rountev, Mariana Sharp, and Guoqing Xu.
- CAV 05: Extended Weighted Pushdown Systems. Akash Lal, Thomas Reps, and Gogul Balakrishnan.
- SAS 03: Weighted Pushdown Systems and Their Application to Interprocedural Dataflow Analysis. Thomas Reps, Stefan Schwoon, and Somesh Jha.
- Precise Interprocedural Dataflow Analysis with Applications to Constant Propagation
- POPL 95: Precise Interprocedural Dataflow Analysis via Graph Reachability. Thomas Reps, Susan Horwitz, and Mooly Sagiv.
