Native C++ Datalog
==================

Lotus provides a native, strongly typed C++17 Datalog runtime under
``Dataflow/Datalog``. The template API lowers rules to a type-erased semantic IR,
then compiles relation dependencies into an SCC-ordered execution plan.

The runtime supports positive recursion, conditions, head expressions, greedy join
planning, runtime bitmask indexes, stratified aggregation and negation, lattice
relations, and bulk-synchronous parallel evaluation.

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

The complete frozen semantic contract is available as
:download:`SEMANTICS.md <datalog/SEMANTICS.md>`.
