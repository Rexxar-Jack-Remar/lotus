#include "IR/UseTraceSSA/Models.h"
#include "IR/UseTraceSSA/SVFGImporter.h"
#include "IR/UseTraceSSA/TemporalHistory.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>

using namespace lotus::usetracessa;
namespace {
TraceFlowGraph fixture(const ObjectSet &objects) {
  Program p;
  auto entry = p.addBlock("entry"), left = p.addBlock("left"),
       right = p.addBlock("right"), join = p.addBlock("join");
  p.addEdge(entry,left); p.addEdge(entry,right); p.addEdge(left,join); p.addEdge(right,join);
  p.addEdge(join,left);
  auto release = p.addOperation(entry,"release");
  auto call = p.addOperation(left,"call");
  auto use = p.addOperation(join,"dereference");
  for (unsigned i = 0; i < 7; ++i) p.addOperation(right,"ordinary use");
  TraceFlowGraph g;
  auto h = TemporalHistory::append(g,0,"caller",p,
      {{release,objects,Event::Release,Certainty::May},
       {use,objects,Event::Dereference,Certainty::Must}});
  Program callee; auto b = callee.addBlock("callee"); callee.addOperation(b,"read");
  auto t = TemporalHistory::append(g,1,"callee",callee,{});
  TemporalHistory::connectCall(g,h,call,7,{t},true,objects);
  return g;
}
TEST(UseTraceSSAObjects, Scaling) {
  FlowStatistics baseline;
  std::size_t nodes = 0, edges = 0, productStates = 0, productEdges = 0, pairs = 0;
  for (unsigned count : {1,10,100,1000,10000}) {
    std::vector<ObjectID> objects(count); std::iota(objects.begin(),objects.end(),1);
    auto g = fixture(ObjectSet::known(objects));
    auto s = g.statistics();
    auto q = queries::useAfterFree(g); q.context = ContextMode::Balanced;
    auto batch = QueryEngine(g).runObjects({q,ObjectUniverse(objects)});
    EXPECT_EQ(batch.found.count(),count);
    const auto &bs = batch.statistics;
    if (count == 1) {
      baseline = s; nodes = g.nodes().size(); edges = g.edges().size();
      productStates = bs.productStates; productEdges = bs.productEdges; pairs = bs.summaryPairs;
    }
    EXPECT_EQ(g.nodes().size(),nodes); EXPECT_EQ(g.edges().size(),edges);
    EXPECT_EQ(s.historyNodes,baseline.historyNodes);
    EXPECT_EQ(s.historyPsiNodes,baseline.historyPsiNodes);
    EXPECT_EQ(s.historyPhiNodes,baseline.historyPhiNodes);
    EXPECT_EQ(bs.productStates,productStates); EXPECT_EQ(bs.productEdges,productEdges);
    EXPECT_EQ(bs.summaryPairs,pairs);
    std::cout << "SCALING objects=" << count << " nodes=" << nodes << " psi="
              << s.historyPsiNodes << " phi=" << s.historyPhiNodes << " edges=" << edges
              << " effects=" << s.guardedEffects << " products=" << bs.productStates
              << " product_edges=" << bs.productEdges << " summaries=" << bs.summaryPairs << '\n';
    // SVFG channel structure is likewise independent of points-to cardinality.
    FunctionLayout f; f.id = 0; auto b = f.control.addBlock("entry");
    auto a = f.control.addOperation(b,"definition"), use = f.control.addOperation(b,"use");
    LocatedSVFGNode source; source.id=1; source.function=0; source.definitionSite=a;
    source.definitionEffects={{Event::Allocate,ObjectSet::known(objects),Certainty::May}};
    LocatedSVFGNode target; target.id=2; target.function=0; target.definitionSite=use;
    LocatedSVFGEdge e; e.id=1; e.from=1; e.to=2; e.objects=ObjectSet::known(objects);
    e.useEffects={{Event::Release,e.objects,Certainty::May}};
    SVFGConstructionInput input; input.functions={f}; input.nodes={source,target}; input.edges={e};
    auto imported = SVFGImporter::build(input);
    EXPECT_EQ(imported.graph.nodes().size(),3u); EXPECT_EQ(imported.graph.edges().size(),2u);
  }
}
TEST(UseTraceSSAObjects, Performance) {
  const char *size = std::getenv("USETRACE_BENCH_OBJECTS");
  const char *mode = std::getenv("USETRACE_BENCH_MODE");
  std::vector<unsigned> sizes = size ? std::vector<unsigned>{unsigned(std::stoul(size))} :
                                     std::vector<unsigned>{1,10,100,1000,10000};
  for (auto count : sizes) {
    std::vector<ObjectID> objects(count); std::iota(objects.begin(),objects.end(),1);
    auto g = fixture(ObjectSet::known(objects));
    auto q = queries::useAfterFree(g); q.context = ContextMode::Balanced;
    using Clock = std::chrono::steady_clock;
    if (!mode || std::string(mode) == "fixed") {
      auto start = Clock::now();
      std::size_t states = 0, edges = 0, pairs = 0;
      for (auto object : objects) {
        q.memoryObject = object;
        auto result = QueryEngine(g).run(q);
        ASSERT_TRUE(result.found());
        states += result.productStates; edges += result.edgesExamined; pairs += result.summaryPairs;
      }
      auto ms = std::chrono::duration<double,std::milli>(Clock::now()-start).count();
      std::cout << "BENCH fixed," << count << ',' << states << ',' << edges << ',' << pairs
                << ",0," << ms << '\n';
    }
    if (!mode || std::string(mode) == "batch") {
      q.memoryObject.reset(); auto start = Clock::now();
      auto result = QueryEngine(g).runObjects({q,ObjectUniverse(objects)});
      auto ms = std::chrono::duration<double,std::milli>(Clock::now()-start).count();
      ASSERT_EQ(result.found.count(),count);
      const auto &s = result.statistics;
      std::cout << "BENCH batch," << count << ',' << s.productStates << ',' << s.edgesExamined
                << ',' << s.summaryPairs << ',' << s.nonemptyDeltas << ',' << ms << '\n';
    }
  }
}
} // namespace
