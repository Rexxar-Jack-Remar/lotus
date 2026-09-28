#include "IR/UFG/DefectDetector.h"
#include "IR/UFG/Search.h"
#include "IR/UseTraceSSA/SVFGBridge.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <algorithm>
#include <random>

using namespace lotus::usetracessa;

namespace {
FlowNodeID addNode(TraceFlowGraph &graph) {
  return graph.addNode(FlowNode{});
}

void addEdge(TraceFlowGraph &graph, FlowNodeID from, FlowNodeID to,
             ObjectSet objects = ObjectSet::unknown(),
             FlowKind kind = FlowKind::History, CallSiteID site = NoNativeID) {
  FlowEdge edge;
  edge.from = from;
  edge.to = to;
  edge.objects = std::move(objects);
  edge.kind = kind;
  edge.callSite = site;
  graph.addEdge(std::move(edge));
}

TEST(UFG, ExplicitLanesMatchFixedObjectQueries) {
  TraceFlowGraph shared;
  auto release = addNode(shared), reset = addNode(shared);
  auto second = addNode(shared), dereference = addNode(shared);
  shared.annotate(release, Event::Release, ObjectSet::known({1, 2}), Certainty::May);
  shared.annotate(reset, Event::Allocate, ObjectSet::known({1}));
  shared.annotate(second, Event::Release, ObjectSet::known({1, 2}), Certainty::May);
  shared.annotate(dereference, Event::Dereference, ObjectSet::known({2}));
  addEdge(shared, release, reset);
  addEdge(shared, reset, second);
  addEdge(shared, second, dereference);
  addEdge(shared, release, dereference, ObjectSet::known({3}));

  lotus::ufg::UFGGraph expanded(shared, {3});
  EXPECT_TRUE(expanded.graph().verify());
  EXPECT_EQ(expanded.statistics().nodes, shared.nodes().size() * 3);
  EXPECT_EQ(expanded.statistics().edges, 10u);
  for (ObjectID object : {1, 2, 3}) {
    for (auto kind : {DefectKind::DoubleFree, DefectKind::UseAfterFree}) {
      Query query = kind == DefectKind::DoubleFree ? queries::doubleFree(shared) :
                                                        queries::useAfterFree(shared);
      query.memoryObject = object;
      auto fixed = QueryEngine(shared).run(query);
      auto lane = expanded.runObject(query, object);
      EXPECT_EQ(lane.status, fixed.status) << "object=" << object;
      for (auto id : lane.nodes) EXPECT_EQ(expanded.objectOf(id), object);
      for (auto id : lane.edges)
        EXPECT_TRUE(expanded.graph().edge(id).objects.contains(object));
    }
  }
  auto scan = lotus::ufg::DefectDetector(expanded).scan(DefectKind::DoubleFree);
  EXPECT_EQ(scan.status, QueryStatus::Found);
  ASSERT_EQ(scan.findings.size(), 1u);
  EXPECT_EQ(scan.findings.front().objects, std::vector<ObjectID>({2}));
  EXPECT_EQ(expanded.originalNode(scan.findings.front().result.nodes.back()), second);
}

TEST(UFG, CallReturnAndUnknownGuard) {
  TraceFlowGraph shared;
  auto release = addNode(shared), entry = addNode(shared);
  auto exit = addNode(shared), dereference = addNode(shared);
  shared.annotate(release, Event::Release, ObjectSet::unknown(), Certainty::May);
  shared.annotate(dereference, Event::Dereference, ObjectSet::known({7}));
  addEdge(shared, release, entry, ObjectSet::unknown(), FlowKind::Call, 10);
  addEdge(shared, entry, exit);
  addEdge(shared, exit, dereference, ObjectSet::known({7}), FlowKind::Return, 10);
  addEdge(shared, exit, dereference, ObjectSet::known({8}), FlowKind::Return, 11);
  lotus::ufg::UFGGraph expanded(shared, {7, 8});
  auto query = queries::useAfterFree(shared);
  for (auto limit : std::vector<std::optional<std::size_t>>{
           std::nullopt, 0u, 1u, 3u}) {
    query.contextLimit = limit;
    auto batch = QueryEngine(shared).runObjects(
        {query, ObjectUniverse({7, 8, UnknownResource})});
    for (ObjectID object : {ObjectID(7), ObjectID(8), UnknownResource}) {
      query.memoryObject = object;
      auto expected = QueryEngine(shared).run(query);
      EXPECT_EQ(expanded.runObject(query, object).status, expected.status);
      EXPECT_EQ(batch.status(object), expected.status);
      query.memoryObject.reset();
    }
  }
  EXPECT_EQ(expanded.runObject(query, 7).status, QueryStatus::Found);
  EXPECT_NE(expanded.runObject(query, 8).status, QueryStatus::Found);
  EXPECT_NE(expanded.runObject(query, UnknownResource).status, QueryStatus::Found);
}

TEST(UFG, OneTabulationCollectsAllSinks) {
  TraceFlowGraph shared;
  auto release = addNode(shared), first = addNode(shared), second = addNode(shared);
  shared.annotate(release, Event::Release, ObjectSet::known({7}));
  shared.annotate(first, Event::Dereference, ObjectSet::known({7}));
  shared.annotate(second, Event::Dereference, ObjectSet::known({7}));
  addEdge(shared, release, first);
  addEdge(shared, first, second);
  lotus::ufg::UFGGraph expanded(shared);
  auto lane = lotus::ufg::SearchEngine(expanded).scan(queries::useAfterFree(shared), 7);
  EXPECT_EQ(lane.status, QueryStatus::Found);
  EXPECT_EQ(lane.foundAt.size(), 2u);
  EXPECT_TRUE(lane.foundAt.count(first));
  EXPECT_TRUE(lane.foundAt.count(second));
  auto report = lotus::ufg::DefectDetector(expanded).scan(DefectKind::UseAfterFree);
  EXPECT_EQ(report.findings.size(), 2u);
  EXPECT_EQ(report.statistics.productStates, lane.productStates);
  EXPECT_EQ(report.statistics.productEdges, lane.productEdges);
}

TEST(UFG, RecursiveCallSummariesAndMismatchedReturns) {
  for (bool matching : {false, true}) {
    TraceFlowGraph shared;
    auto release = addNode(shared), entry = addNode(shared);
    auto exit = addNode(shared), sink = addNode(shared);
    shared.annotate(release, Event::Release, ObjectSet::known({7}));
    shared.annotate(sink, Event::Dereference, ObjectSet::known({7}));
    addEdge(shared, release, entry, ObjectSet::unknown(), FlowKind::Call, 10);
    addEdge(shared, entry, exit);
    addEdge(shared, entry, entry, ObjectSet::unknown(), FlowKind::Call, 20);
    addEdge(shared, exit, exit, ObjectSet::unknown(), FlowKind::Return, 20);
    addEdge(shared, exit, sink, ObjectSet::unknown(), FlowKind::Return,
            matching ? 10 : 99);
    lotus::ufg::UFGGraph expanded(shared);
    auto query = queries::useAfterFree(shared);
    query.context = ContextMode::Balanced;
    query.memoryObject = 7;
    for (auto limit : std::vector<std::optional<std::size_t>>{
             std::nullopt, 0u, 1u, 3u}) {
      query.contextLimit = limit;
      auto reference = QueryEngine(shared).run(query);
      auto actual = expanded.runObject(query, 7);
      EXPECT_EQ(actual.status, reference.status);
      if (!limit.has_value()) EXPECT_EQ(actual.found(), matching);
      if (!matching && limit && *limit == 0) EXPECT_TRUE(actual.found());
      if (matching) EXPECT_TRUE(actual.found());
      EXPECT_GT(actual.summaryPairs, 0u);
    }
  }
}

TEST(UFG, RandomFixedObjectDifferential) {
  std::mt19937 random(923817);
  for (unsigned trial = 0; trial < 80; ++trial) {
    SCOPED_TRACE(trial);
    TraceFlowGraph shared;
    std::vector<FlowNodeID> nodes;
    for (unsigned i = 0; i < 7; ++i) nodes.push_back(addNode(shared));
    auto guard = [&]() {
      if (random() % 7 == 0) return ObjectSet::unknown();
      std::vector<ObjectID> objects;
      for (ObjectID object : {1, 2, 3}) if (random() % 2) objects.push_back(object);
      return ObjectSet::known(std::move(objects));
    };
    for (unsigned i = 0; i < nodes.size(); ++i) {
      Event event = i % 3 == 0 ? Event::Release :
                    i % 3 == 1 ? Event::Dereference : Event::Allocate;
      shared.annotate(nodes[i], event, guard(),
                      random() % 2 ? Certainty::Must : Certainty::May);
      addEdge(shared, nodes[i], nodes[(i + 1) % nodes.size()], guard());
      if (random() % 2)
        addEdge(shared, nodes[i], nodes[random() % nodes.size()], guard());
    }
    lotus::ufg::UFGGraph expanded(shared, {1, 2, 3});
    auto query = queries::useAfterFree(shared);
    query.requireNonEmpty = trial % 5 == 0;
    for (ObjectID object : {1, 2, 3}) {
      query.memoryObject = object;
      auto expected = QueryEngine(shared).run(query);
      auto actual = expanded.runObject(query, object);
      EXPECT_EQ(actual.status, expected.status) << "object=" << object;
      if (actual.found()) {
        ASSERT_FALSE(actual.nodes.empty());
        for (auto node : actual.nodes) EXPECT_EQ(expanded.objectOf(node), object);
        EXPECT_NE(std::find(query.sinks.begin(), query.sinks.end(),
                            expanded.originalNode(actual.nodes.back())), query.sinks.end());
      }
    }
  }
}

TEST(UFG, BudgetsAndIncompleteModelsDoNotProveAbsence) {
  TraceFlowGraph shared;
  auto release = addNode(shared), middle = addNode(shared), sink = addNode(shared);
  shared.annotate(release, Event::Release, ObjectSet::known({1}));
  shared.annotate(sink, Event::Dereference, ObjectSet::known({1}));
  addEdge(shared, release, middle);
  addEdge(shared, middle, sink);
  lotus::ufg::UFGGraph expanded(shared);
  auto query = queries::useAfterFree(shared);
  query.maxProductStates = 2;
  EXPECT_EQ(expanded.runObject(query, 1).status, QueryStatus::Unknown);

  query.maxProductStates = 0;
  auto found = expanded.runObject(query, 1);
  EXPECT_EQ(found.status, QueryStatus::Found);
  EXPECT_EQ(found.edges.size(), 2u);

  shared.addIssue("unknown library effect");
  lotus::ufg::UFGGraph incomplete(shared);
  auto negative = queries::doubleFree(shared);
  EXPECT_EQ(incomplete.runObject(negative, 1).status, QueryStatus::Unknown);
}

TEST(UFG, RandomContextDifferential) {
  std::mt19937 random(1234567);
  for (unsigned trial = 0; trial < 60; ++trial) {
    SCOPED_TRACE(trial);
    TraceFlowGraph shared;
    std::vector<FlowNodeID> nodes;
    for (unsigned i = 0; i < 5; ++i) nodes.push_back(addNode(shared));
    shared.annotate(nodes.front(), Event::Release, ObjectSet::known({1}));
    shared.annotate(nodes.back(), Event::Dereference, ObjectSet::known({1}));
    for (unsigned i = 0; i < 10; ++i) {
      auto kind = random() % 3 == 0 ? FlowKind::Call :
                  random() % 3 == 0 ? FlowKind::Return : FlowKind::History;
      addEdge(shared, nodes[random() % nodes.size()], nodes[random() % nodes.size()],
              ObjectSet::unknown(), kind,
              kind == FlowKind::History ? NoNativeID : 1 + random() % 2);
    }
    lotus::ufg::UFGGraph expanded(shared);
    auto query = queries::useAfterFree(shared);
    query.memoryObject = 1;
    for (auto context : {ContextMode::Balanced, ContextMode::Realizable}) {
      query.context = context;
      for (auto limit : std::vector<std::optional<std::size_t>>{
               std::nullopt, 0u, 1u, 3u}) {
        query.contextLimit = limit;
        auto expected = QueryEngine(shared).run(query);
        EXPECT_EQ(expanded.runObject(query, 1).status, expected.status);
        query.memoryObject.reset();
        auto batch = QueryEngine(shared).runObjects({query, ObjectUniverse({1})});
        EXPECT_EQ(batch.status(1), expected.status);
        query.memoryObject = 1;
      }
    }
  }
}

TEST(UFG, NativeFrontendFindingsAgree) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"IR(
    declare void @free(i8*)
    define i8 @check(i8* %p) {
      call void @free(i8* %p)
      call void @free(i8* %p)
      %value = load i8, i8* %p
      ret i8 %value
    }
  )IR", diagnostic, context);
  ASSERT_TRUE(module);
  lotus::analysis::SVFG svfg;
  svfg.addObjectForValue(module->getFunction("check")->getArg(0), 42);
  for (auto kind : {DefectKind::DoubleFree, DefectKind::UseAfterFree}) {
    auto mode = kind == DefectKind::DoubleFree ? NativeHistoryMode::DoubleFree :
                                                 NativeHistoryMode::UseAfterFree;
    auto input = buildUseTraceSSAFromLotusSVFG(svfg, *module, mode);
    lotus::ufg::UFGGraph expanded(input.graph);
    for (auto limit : std::vector<std::optional<std::size_t>>{
             std::nullopt, 0u, 3u}) {
      auto factored = lotus::usetracessa::DefectDetector(input.graph, limit).scan(kind);
      auto baseline = lotus::ufg::DefectDetector(expanded, limit).scan(kind);
      EXPECT_EQ(baseline.status, factored.status);
      EXPECT_EQ(baseline.findings.size(), factored.findings.size());
      ASSERT_FALSE(baseline.findings.empty());
      EXPECT_EQ(baseline.findings.front().objects, factored.findings.front().objects);
      EXPECT_EQ(baseline.findings.front().witnessObject,
                factored.findings.front().witnessObject);
    }
  }
}

TEST(UFG, GenericDetectorsUseOwnTabulation) {
  TraceFlowGraph shared;
  auto source = addNode(shared), check = addNode(shared), sink = addNode(shared);
  shared.annotate(source, Event::Source);
  shared.annotate(sink, Event::Sink);
  addEdge(shared, source, check);
  addEdge(shared, check, sink);
  lotus::ufg::UFGGraph expanded(shared);
  auto taint = lotus::ufg::DefectDetector(expanded).run(DefectKind::Taint);
  EXPECT_EQ(taint.result.status, QueryStatus::Found);
  EXPECT_EQ(taint.result.nodes.front(), source);
  EXPECT_EQ(taint.result.nodes.back(), sink);
  EXPECT_EQ(taint.result.status,
            lotus::usetracessa::DefectDetector(shared).run(DefectKind::Taint).result.status);

  shared.annotate(check, Event::Sanitize | Event::NonNull);
  lotus::ufg::UFGGraph trapped(shared);
  auto detector = lotus::ufg::DefectDetector(trapped);
  EXPECT_EQ(detector.run(DefectKind::Taint).result.status, QueryStatus::NotFound);
  EXPECT_EQ(detector.run(DefectKind::UncheckedUse, {source}, {sink}).result.status,
            QueryStatus::NotFound);
  EXPECT_EQ(detector.run(DefectKind::UncheckedUse, {source}, {sink}).result.status,
            lotus::usetracessa::DefectDetector(shared)
                .run(DefectKind::UncheckedUse, {source}, {sink}).result.status);
}

TEST(UFG, LeakAutomataShareSearchSemantics) {
  for (bool file : {false, true}) {
    TraceFlowGraph shared;
    auto acquire = addNode(shared), release = addNode(shared);
    auto exit = addNode(shared);
    shared.annotate(acquire, file ? Event::Open : Event::Allocate,
                    ObjectSet::known({7}), Certainty::May);
    shared.annotate(release, file ? Event::Close : Event::Release,
                    ObjectSet::known({7}), Certainty::Must);
    shared.annotate(exit, Event::Exit);
    addEdge(shared, acquire, release);
    addEdge(shared, release, exit);
    DefectKind kind = file ? DefectKind::FileLeak : DefectKind::MemoryLeak;
    lotus::ufg::UFGGraph closed(shared);
    EXPECT_EQ(lotus::usetracessa::DefectDetector(shared).scan(kind).status,
              QueryStatus::NotFound);
    EXPECT_EQ(lotus::ufg::DefectDetector(closed).scan(kind).status,
              QueryStatus::NotFound);

    TraceFlowGraph leaking;
    auto open = addNode(leaking), end = addNode(leaking);
    leaking.annotate(open, file ? Event::Open : Event::Allocate,
                     ObjectSet::known({7}), Certainty::May);
    leaking.annotate(end, Event::Exit);
    addEdge(leaking, open, end);
    lotus::ufg::UFGGraph expanded(leaking);
    auto factored = lotus::usetracessa::DefectDetector(leaking).scan(kind);
    auto baseline = lotus::ufg::DefectDetector(expanded).scan(kind);
    EXPECT_EQ(factored.status, QueryStatus::Found);
    EXPECT_EQ(baseline.status, factored.status);
    ASSERT_EQ(baseline.findings.size(), 1u);
    EXPECT_EQ(baseline.findings.front().objects, std::vector<ObjectID>({7}));
  }
}

TEST(UFG, NativeLeakModelsDetectMissingCloseAndFree) {
  for (bool closed : {false, true}) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    const char *ir = closed ? R"IR(
      declare i8* @malloc(i64)
      declare void @free(i8*)
      declare i8* @fopen(i8*, i8*)
      declare i32 @fclose(i8*)
      define i32 @main() {
        %p = call i8* @malloc(i64 8)
        call void @free(i8* %p)
        %f = call i8* @fopen(i8* null, i8* null)
        %r = call i32 @fclose(i8* %f)
        ret i32 0
      }
    )IR" : R"IR(
      declare i8* @malloc(i64)
      declare i8* @fopen(i8*, i8*)
      define i32 @main() {
        %p = call i8* @malloc(i64 8)
        %f = call i8* @fopen(i8* null, i8* null)
        ret i32 0
      }
    )IR";
    auto module = llvm::parseAssemblyString(ir, diagnostic, context);
    ASSERT_TRUE(module);
    lotus::analysis::SVFG svfg;
    for (auto kind : {DefectKind::MemoryLeak, DefectKind::FileLeak}) {
      auto mode = kind == DefectKind::MemoryLeak ? NativeHistoryMode::MemoryLeak :
                                                   NativeHistoryMode::FileLeak;
      auto imported = buildUseTraceSSAFromLotusSVFG(svfg, *module, mode);
      lotus::ufg::UFGGraph expanded(imported.graph);
      auto factored = lotus::usetracessa::DefectDetector(imported.graph).scan(kind);
      auto baseline = lotus::ufg::DefectDetector(expanded).scan(kind);
      EXPECT_EQ(baseline.status, factored.status);
      EXPECT_EQ(baseline.findings.empty(), closed);
      EXPECT_EQ(factored.findings.empty(), closed);
    }
  }
}

TEST(UFG, LeakQueriesReachCallerExit) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"IR(
    declare i8* @malloc(i64)
    define void @allocate() {
      %p = call i8* @malloc(i64 8)
      ret void
    }
    define i32 @main() {
      call void @allocate()
      ret i32 0
    }
  )IR", diagnostic, context);
  ASSERT_TRUE(module);
  lotus::analysis::SVFG svfg;
  auto imported = buildUseTraceSSAFromLotusSVFG(
      svfg, *module, NativeHistoryMode::MemoryLeak);
  lotus::ufg::UFGGraph expanded(imported.graph);
  auto check = [](const DefectScan &scan, const TraceFlowGraph &graph) {
    EXPECT_EQ(scan.status, QueryStatus::Found);
    ASSERT_EQ(scan.findings.size(), 1u);
    bool returned = false;
    for (auto edge : scan.findings.front().result.edges)
      returned |= graph.edge(edge).kind == FlowKind::Return;
    EXPECT_TRUE(returned);
  };
  for (auto limit : std::vector<std::optional<std::size_t>>{std::nullopt, 3u}) {
    check(lotus::usetracessa::DefectDetector(imported.graph, limit)
              .scan(DefectKind::MemoryLeak), imported.graph);
    check(lotus::ufg::DefectDetector(expanded, limit).scan(DefectKind::MemoryLeak),
          expanded.graph());
  }
}

TEST(UFG, RootReturnsTransferDirectlyAcquiredResources) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"IR(
    declare i8* @malloc(i64)
    declare i8* @fopen(i8*, i8*)
    define i8* @allocate() {
      %p = call i8* @malloc(i64 8)
      ret i8* %p
    }
    define i8* @open_file() {
      %f = call i8* @fopen(i8* null, i8* null)
      ret i8* %f
    }
  )IR", diagnostic, context);
  ASSERT_TRUE(module);
  lotus::analysis::SVFG svfg;
  for (auto kind : {DefectKind::MemoryLeak, DefectKind::FileLeak}) {
    auto mode = kind == DefectKind::MemoryLeak ? NativeHistoryMode::MemoryLeak :
                                                 NativeHistoryMode::FileLeak;
    auto imported = buildUseTraceSSAFromLotusSVFG(svfg, *module, mode);
    lotus::ufg::UFGGraph expanded(imported.graph);
    EXPECT_TRUE(lotus::usetracessa::DefectDetector(imported.graph)
                    .scan(kind).findings.empty());
    EXPECT_TRUE(lotus::ufg::DefectDetector(expanded).scan(kind).findings.empty());
  }
}
} // namespace
