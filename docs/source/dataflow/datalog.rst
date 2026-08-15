Native C++ Datalog
==================

Lotus provides a native, strongly typed C++17 Datalog runtime under
``Dataflow/Datalog``. The template API lowers rules to a type-erased semantic IR,
then compiles relation dependencies into an SCC-ordered execution plan.

The runtime supports positive recursion, conditions, head expressions,
distinct-aware greedy join planning, specialized full and partial runtime indexes,
stratified aggregation and negation, lattice relations, and bulk-synchronous
parallel evaluation.

.. code-block:: cpp

   #include "Dataflow/Datalog/Ascent.h"

   using namespace lotus::datalog;

   context ctx;
   auto edge = ctx.relation<int, int>("edge");
   auto path = ctx.relation<int, int>("path");
   auto x = ctx.var<int>("x");
   auto y = ctx.var<int>("y");
   auto z = ctx.var<int>("z");

   program p(ctx);
   p.rule(path(x, y), edge(x, y));
   p.rule(path(x, z), path(x, y) && edge(y, z));

   edge.insert(1, 2);
   edge.insert(2, 3);
   auto compiled = p.compile();
   ExecutionOptions options;
   options.worker_count = 4;
   compiled.run(options);

``Datalog.h`` and ``Ascent.h`` are umbrella headers. The API is also split into
focused headers such as ``Context.h``, ``Relation.h``, ``Aggregate.h``,
``Lattice.h``, ``Program.h``, and ``Scheduler.h``.

The aggregate API provides collecting, streaming, and reducible factories.
Reducible aggregators run with worker-local state. The lattice library includes
minimum, maximum, set-union, dual, product, bounded-set, and
constant-propagation values.

The current semantic and architecture reference is maintained in
``lib/Dataflow/Datalog/README.md``.

Command-line engine
-------------------

``lotus-datalog`` is the engine entry point for non-C++ clients. It consumes the
same type-erased Semantic IR as JSON; it is not a benchmark-specific front-end.

.. code-block:: bash

   ./build/bin/lotus-datalog schema > transitive-closure.json
   ./build/bin/lotus-datalog validate transitive-closure.json
   ./build/bin/lotus-datalog run transitive-closure.json --workers 4 --pretty
   ./build/bin/lotus-datalog schema | ./build/bin/lotus-datalog run -

The JSON format supports set and lattice relations, facts, multiple rule heads,
positive and negative atoms, filters, expressions, and built-in reducible
aggregates. Output rows are sorted deterministically, making the CLI suitable for
future Python differential and performance harnesses. The complete format is
documented in ``tools/dataflow/datalog/README.md``.
