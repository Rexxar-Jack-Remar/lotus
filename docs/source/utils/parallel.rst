Parallel Execution Infrastructure
=================================

``include/Utils/Parallel/`` and ``lib/Utils/Parallel/`` provide the shared
thread-pool and scheduling utilities used by parallel analyses.

**Main components**:

- ``ThreadPool`` for task execution and parallel loops.
- ``CancellationToken`` and ``CancellationSource`` for cooperative stop.
- ``PipelineScheduler`` and ``ParallelSchedulerPass`` for structured pipelines.
- ``Task`` and thread-local reducer helpers for work coordination.

Use this layer when an analysis needs bounded parallelism without embedding its
own scheduler.

See also :doc:`utilities`.
