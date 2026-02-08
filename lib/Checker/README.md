# Checker Library

Bug checkers and reporting infrastructure.

| Subdir | Purpose |
|--------|---------|
| **Concurrency** | Thread-safety: Atomicity, ConditionVariable, DataRace, Deadlock, LockMismatch. |
| **FiTx** | Detectors: double-free, double-lock/unlock, leak, null-ptr, ref/ unref, UAF, use-before-init. |
| **GVFA** | Global value-flow based. Sources/sinks over Dyck analysis. Checkers: NullPointer, UseAfterFree, UseOfUninitializedVariable, InvalidUseOfStackAddress, FreeOfNonHeapMemory. |
| **KINT** | Integer bug detection. Range analysis, taint analysis, SMT (Z3). |
| **Pulse** | Biabductive analysis (Infer Pulse–style). Witnessable bugs, disjunctive domain, loop abstraction. |
| **Report** | Shared reporting: BugReport, BugReportMgr, BugTypes, SARIF, SuppressionManager. |
