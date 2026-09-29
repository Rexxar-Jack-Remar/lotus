#include "IR/UseTraceSSA/Models.h"
#include "IR/UseTraceSSA/DefectDetector.h"
#include <sstream>
#include "IR/UseTraceSSA/SVFGImporter.h"
#include "IR/UseTraceSSA/TemporalHistory.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>

using namespace lotus::usetracessa;
namespace {
FlowNodeID node(TraceFlowGraph &g, Event e = Event::None) {
  FlowNode n; n.events = e; return g.addNode(n);
}
void edge(TraceFlowGraph &g, ID a, ID b, ObjectSet objects = ObjectSet::unknown(),
          FlowKind kind = FlowKind::History, CallSiteID site = NoNativeID) {
  FlowEdge e; e.from = a; e.to = b; e.objects = objects; e.kind = kind; e.callSite = site;
  g.addEdge(e);
}
Query query(ID a, ID b) { Query q; q.sources = {a}; q.sinks = {b}; return q; }
void checkWitness(const TraceFlowGraph &g, const Query &q, ObjectID object,
                  FlowNodeID sink, const QueryResult &witness) {
  ASSERT_TRUE(witness.found());
  ASSERT_FALSE(witness.nodes.empty());
  ASSERT_EQ(witness.edges.size() + 1, witness.nodes.size());
  ASSERT_EQ(witness.automatonStates.size(), witness.nodes.size());
  EXPECT_NE(std::find(q.sources.begin(), q.sources.end(), witness.nodes.front()), q.sources.end());
  std::vector<CallSiteID> calls;
  bool truncated = false;
  for (std::size_t i = 0; i < witness.nodes.size(); ++i) {
    auto node = witness.nodes[i];
    EXPECT_EQ(std::find(q.traps.begin(), q.traps.end(), node), q.traps.end());
    auto effective = g.effectiveEvent(node, object);
    ID before = i ? witness.automatonStates[i - 1] : q.automaton.initial;
    ID after = witness.automatonStates[i];
    bool valid = effective.certainty == Certainty::May && before == after;
    if (!hasEvent(effective.events, q.trapEvents))
      valid |= after == (q.automaton.transition ?
                         q.automaton.transition(before, effective.events) : before);
    EXPECT_TRUE(valid);
    if (!i) continue;
    const auto &edge = g.edge(witness.edges[i - 1]);
    EXPECT_EQ(edge.from, witness.nodes[i - 1]);
    EXPECT_EQ(edge.to, node);
    EXPECT_TRUE(edge.enabled);
    EXPECT_TRUE(edge.objects.contains(object));
    if (q.context == ContextMode::Insensitive) continue;
    if (edge.kind == FlowKind::Call) {
      if (q.contextLimit && calls.size() >= *q.contextLimit) {
        if (!calls.empty()) calls.erase(calls.begin());
        truncated = true;
      }
      calls.push_back(edge.callSite);
    } else if (edge.kind == FlowKind::Return) {
      if (calls.empty()) EXPECT_TRUE(q.context != ContextMode::Balanced || truncated);
      else { EXPECT_EQ(calls.back(), edge.callSite); calls.pop_back(); }
    }
  }
  if (witness.witnessComplete) {
    EXPECT_EQ(witness.nodes.back(), sink);
    EXPECT_NE(std::find(q.automaton.accepting.begin(), q.automaton.accepting.end(),
                        witness.automatonStates.back()), q.automaton.accepting.end());
    if (q.context == ContextMode::Balanced) EXPECT_TRUE(calls.empty());
    if (q.requireNonEmpty) EXPECT_FALSE(witness.edges.empty());
  }
}
ObjectBatchResult compare(const TraceFlowGraph &g, Query q,
                           std::vector<ObjectID> objects = {1, 2, 3, 4, UnknownResource}) {
  q.memoryObject.reset();
  auto batch = QueryEngine(g).runObjects({q, ObjectUniverse(objects), true});
  for (auto object : objects) {
    q.memoryObject = object;
    auto fixed = QueryEngine(g).run(q);
    EXPECT_EQ(batch.status(object), fixed.status) << "object=" << object;
    auto allSinks = QueryEngine(g).runToSinks(q);
    EXPECT_EQ(allSinks.status, fixed.status);
    for (auto sink : q.sinks) {
      if (batch.hasWitnesses()) {
        auto accepted = batch.foundAt.find(sink);
        bool found = accepted != batch.foundAt.end() &&
                     accepted->second.test(batch.universe.index(object));
        auto witness = batch.witness(sink, object);
        EXPECT_EQ(witness.found(), found);
        if (found) checkWitness(g, q, object, sink, witness);
      }
      Query single = q;
      single.sinks = {sink};
      auto expected = QueryEngine(g).run(single);
      auto found = allSinks.foundAt.find(sink);
      EXPECT_EQ(found != allSinks.foundAt.end(), expected.found());
      if (found != allSinks.foundAt.end()) {
        EXPECT_EQ(found->second.nodes, expected.nodes);
        EXPECT_EQ(found->second.edges, expected.edges);
        EXPECT_EQ(found->second.automatonStates, expected.automatonStates);
        EXPECT_EQ(found->second.witnessComplete, expected.witnessComplete);
      }
    }
    if (fixed.found()) {
      EXPECT_FALSE(fixed.nodes.empty());
      for (auto eid : fixed.edges) EXPECT_TRUE(g.edge(eid).objects.contains(object));
    }
  }
  EXPECT_EQ(batch.found.count() + batch.notFound.count() + batch.unknown.count(),
            batch.universe.objects().size());
  return batch;
}
TEST(UseTraceSSAObjects, SequentialAndJoin) {
  for (bool overlap : {false, true}) {
    TraceFlowGraph g; auto a = node(g), b = node(g), c = node(g);
    edge(g, a, b, ObjectSet::known(overlap ? std::vector<ObjectID>{1,2} :
                                                         std::vector<ObjectID>{1}));
    edge(g, b, c, ObjectSet::known(overlap ? std::vector<ObjectID>{2,3} :
                                                         std::vector<ObjectID>{2}));
    auto q = query(a, c); auto r = compare(g, q);
    EXPECT_EQ(r.found.count(), overlap ? 1u : 0u);
    EXPECT_TRUE(QueryEngine(g).run(q).found()); // Generic taint may change objects.
    edge(g, a, c, ObjectSet::known({1}));
    edge(g, a, c, ObjectSet::known({3}));
    r = compare(g, q); EXPECT_EQ(r.found.count(), overlap ? 3u : 2u);
  }
}
TEST(UseTraceSSAObjects, DefaultBudgetsAreUnlimitedAndContextDepthIsThree) {
  Query q;
  ASSERT_TRUE(q.contextLimit);
  EXPECT_EQ(*q.contextLimit, 3u);
  EXPECT_EQ(q.maxProductStates, 0u);
  EXPECT_EQ(q.maxSummaryPairs, 0u);
  TraceFlowGraph g;
  auto first = node(g), last = first;
  for (unsigned i = 0; i < 100000; ++i) {
    auto next = node(g); edge(g, last, next); last = next;
  }
  q.sources = {first}; q.sinks = {last};
  auto result = QueryEngine(g).runObjects({q, ObjectUniverse({1})});
  EXPECT_EQ(result.status(1), QueryStatus::Found);
  EXPECT_GT(result.statistics.productStates, 100000u);
  EXPECT_TRUE(result.completion.searchComplete);
}

TEST(UseTraceSSAObjects, BudgetAndModelCompletenessAreIndependent) {
  TraceFlowGraph g;
  auto a = node(g), b = node(g), c = node(g);
  edge(g, a, b); edge(g, b, c);
  for (auto reason : {SearchStopReason::ProductStates, SearchStopReason::SummaryPairs}) {
    auto q = query(a, c);
    if (reason == SearchStopReason::ProductStates) q.maxProductStates = 1;
    if (reason == SearchStopReason::SummaryPairs) q.maxSummaryPairs = 1;
    auto check = [&](const SearchCompletion &completion) {
      EXPECT_FALSE(completion.searchComplete);
      EXPECT_TRUE(completion.modelComplete);
      EXPECT_EQ(completion.stopReason, reason);
      EXPECT_EQ(completion.budgetLimit, 1u);
      EXPECT_EQ(completion.budgetObserved, 1u);
    };
    check(QueryEngine(g).run(q).completion);
    check(QueryEngine(g).runToSinks(q).completion);
    check(QueryEngine(g).runObjects({q, ObjectUniverse({1})}).completion);
  }
  auto q = query(a, c);
  q.sinks.push_back(a); q.maxProductStates = 1;
  auto partial = QueryEngine(g).runObjects({q, ObjectUniverse({1}), true});
  EXPECT_EQ(partial.status(1), QueryStatus::Found);
  EXPECT_FALSE(partial.completion.searchComplete);
  EXPECT_TRUE(partial.witness(a, 1).found());
  EXPECT_FALSE(partial.witness(a, 1).completion.searchComplete);
  g.addIssue("missing external model");
  q.maxProductStates = 0;
  auto incompleteModel = QueryEngine(g).runObjects({q, ObjectUniverse({1})});
  EXPECT_TRUE(incompleteModel.completion.searchComplete);
  EXPECT_FALSE(incompleteModel.completion.modelComplete);
  EXPECT_FALSE(incompleteModel.complete);
  q.maxProductStates = 1;
  auto both = QueryEngine(g).runObjects({q, ObjectUniverse({1})});
  EXPECT_FALSE(both.completion.searchComplete);
  EXPECT_FALSE(both.completion.modelComplete);
}

TEST(UseTraceSSAObjects, InsensitiveModeIsDistinctFromLegacyZeroDepth) {
  TraceFlowGraph g;
  auto a = node(g), b = node(g), c = node(g);
  edge(g, a, b, ObjectSet::unknown(), FlowKind::Call, 7);
  edge(g, b, c, ObjectSet::unknown(), FlowKind::Return, 8);
  auto q = query(a, c); q.contextLimit = 0;
  EXPECT_FALSE(QueryEngine(g).run(q).found());
  q.context = ContextMode::Insensitive;
  EXPECT_TRUE(QueryEngine(g).run(q).found());
}
TEST(UseTraceSSAObjects, FragmentedDeltasExpandEachProductOnce) {
  TraceFlowGraph g;
  auto join = node(g), sink = node(g), thread = node(g);
  edge(g, join, sink);
  edge(g, join, thread, ObjectSet::unknown(), FlowKind::Thread);
  Query q;
  q.sinks = {sink};
  q.context = ContextMode::Insensitive;
  std::vector<ObjectID> objects;
  for (ObjectID object = 1; object <= 32; ++object) {
    auto source = node(g);
    q.sources.push_back(source);
    objects.push_back(object);
    edge(g, source, join, ObjectSet::known({object}));
  }
  auto batch = QueryEngine(g).runObjects({q, ObjectUniverse(objects)});
  EXPECT_EQ(batch.found.count(), objects.size());
  EXPECT_EQ(batch.statistics.edgesExamined, objects.size() + 2);
  EXPECT_EQ(batch.statistics.productEdges, objects.size() + 1);

  q.sinks = {thread};
  batch = QueryEngine(g).runObjects({q, ObjectUniverse(objects)});
  EXPECT_EQ(batch.unknown.count(), objects.size());
  EXPECT_TRUE(batch.found.none());
}
TEST(UseTraceSSAObjects, GuardedEventsAndCertainty) {
  TraceFlowGraph g; auto a = node(g), b = node(g);
  edge(g, a, b);
  g.annotate(a, Event::Release, ObjectSet::known({1,2}));
  g.annotate(b, Event::Dereference, ObjectSet::known({2,3}));
  auto q = query(a, b); q.automaton = Automaton::useAfterFree();
  EXPECT_EQ(compare(g, q).found.count(), 1u);
  // Overlapping effects at ONE node must be combined, not sequentially applied.
  g.annotate(b, Event::Release, ObjectSet::known({3,4}));
  EXPECT_EQ(g.effectiveEvent(b, 3).events, Event::Release | Event::Dereference);
  EXPECT_EQ(compare(g, q).found.count(), 1u);

  TraceFlowGraph h; auto x = node(h, Event::Release), reset = node(h), y = node(h, Event::Release);
  edge(h, x, reset); edge(h, reset, y);
  h.annotate(reset, Event::Allocate, ObjectSet::known({1}), Certainty::May);
  h.annotate(reset, Event::Allocate, ObjectSet::known({2}), Certainty::Must);
  h.annotate(reset, Event::Allocate, ObjectSet::known({}), Certainty::Must);
  auto df = queries::doubleFree(h);
  auto r = compare(h, df); EXPECT_EQ(r.status(1), QueryStatus::Found);
  EXPECT_EQ(r.status(2), QueryStatus::NotFound);
  h.annotate(reset, Event::Release, ObjectSet::unknown(), Certainty::Must);
  EXPECT_EQ(h.effectiveEvent(reset, 2).certainty, Certainty::May);
  EXPECT_EQ(compare(h, df).status(2), QueryStatus::Found);
  EXPECT_TRUE(h.complete());
}
TEST(UseTraceSSAObjects, SharedMasksAcrossWordBoundaries) {
  for (unsigned count : {0, 1, 63, 64, 65, 257}) {
    SCOPED_TRACE(count);
    std::vector<ObjectID> objects(count), even, odd, reset, used;
    std::iota(objects.begin(), objects.end(), 1);
    for (auto object : objects) {
      (object % 2 ? odd : even).push_back(object);
      if (object % 3 == 0) reset.push_back(object);
      if (object % 5 != 0) used.push_back(object);
    }
    TraceFlowGraph g;
    auto root = node(g, Event::Release), left = node(g), right = node(g), join = node(g);
    edge(g, root, left, ObjectSet::known(even), FlowKind::Call, 7);
    edge(g, root, right, ObjectSet::known(odd), FlowKind::Call, 8);
    edge(g, left, join, ObjectSet::unknown(), FlowKind::Return, 7);
    edge(g, right, join, ObjectSet::unknown(), FlowKind::Return, 8);
    edge(g, join, root);
    g.annotate(left, Event::Allocate, ObjectSet::known(reset));
    g.annotate(join, Event::Dereference, ObjectSet::known(used));
    for (auto limit : {std::optional<std::size_t>{}, std::optional<std::size_t>{3}}) {
      auto q = queries::useAfterFree(g);
      q.context = ContextMode::Balanced;
      q.contextLimit = limit;
      auto batch = compare(g, q, objects);
      EXPECT_TRUE(batch.complete);
      EXPECT_TRUE(batch.unknown.none());
      for (auto object : objects)
        EXPECT_EQ(batch.status(object), object % 5 != 0 && object % 6 != 0 ?
                                       QueryStatus::Found : QueryStatus::NotFound);
    }
  }
}
TEST(UseTraceSSAObjects, UnknownAndEmptyTraps) {
  for (auto objects : {ObjectSet::unknown(), ObjectSet::known({}), ObjectSet::known({1})}) {
    TraceFlowGraph g; auto a = node(g), b = node(g), c = node(g);
    edge(g, a, b); edge(g, b, c);
    g.annotate(b, Event::Sanitize, objects);
    auto q = query(a, c); q.trapEvents = Event::Sanitize;
    auto r = compare(g, q);
    EXPECT_EQ(r.status(1), objects.isUnknown() || objects.empty() ?
                          QueryStatus::Found : QueryStatus::NotFound);
  }
}
TEST(UseTraceSSAObjects, ContextAndPositiveCycles) {
  TraceFlowGraph g; auto a = node(g), b = node(g), c = node(g);
  edge(g, a, b, ObjectSet::known({1,2}), FlowKind::Call, 7);
  edge(g, b, c, ObjectSet::known({2,3}), FlowKind::Return, 7);
  edge(g, b, b, ObjectSet::unknown(), FlowKind::Call, 8);
  edge(g, b, b, ObjectSet::known({2}), FlowKind::Return, 8);
  auto q = query(a,c); q.context = ContextMode::Balanced;
  EXPECT_EQ(compare(g,q).found.count(), 1u);
  edge(g, b, c, ObjectSet::known({1}), FlowKind::Return, 9);
  EXPECT_EQ(compare(g,q).found.count(), 1u); // Wrong return never repairs object 1.
  q.context = ContextMode::Realizable; EXPECT_EQ(compare(g,q).found.count(), 1u);
  q.context = ContextMode::Insensitive; EXPECT_EQ(compare(g,q).found.count(), 2u);
  edge(g, c, a, ObjectSet::known({2}));
  q = query(a,a); q.context = ContextMode::Balanced; q.requireNonEmpty = true;
  auto r = compare(g,q); EXPECT_EQ(r.found.count(), 1u);
  q.requireNonEmpty = false; EXPECT_EQ(compare(g,q).found.count(), 5u);
}
TEST(UseTraceSSAObjects, AllSinkContextsAndWitnessLimits) {
  TraceFlowGraph g;
  auto source = node(g), inside = node(g), sink = node(g), wrong = node(g);
  edge(g, source, inside, ObjectSet::known({1, 2}), FlowKind::Call, 7);
  edge(g, inside, sink, ObjectSet::known({2}), FlowKind::Return, 7);
  edge(g, inside, wrong, ObjectSet::known({1, 2}), FlowKind::Return, 8);
  edge(g, inside, inside, ObjectSet::unknown(), FlowKind::Call, 7);
  for (auto context : {ContextMode::Insensitive, ContextMode::Balanced, ContextMode::Realizable}) {
    for (auto limit : {std::optional<std::size_t>{}, std::optional<std::size_t>{0},
                       std::optional<std::size_t>{1}, std::optional<std::size_t>{3},
                       std::optional<std::size_t>{6}}) {
      Query q = query(source, sink);
      q.sinks = {source, inside, sink, wrong, sink};
      q.context = context;
      q.contextLimit = limit;
      for (bool nonempty : {false, true}) {
        q.requireNonEmpty = nonempty;
        for (auto maxWitness : {std::size_t(1), std::size_t(100)}) {
          q.maxWitnessEdges = maxWitness;
          compare(g, q, {1, 2});
          auto all = QueryEngine(g).runToSinks(q);
          EXPECT_TRUE(all.complete);
          for (auto sink : q.sinks) {
            Query single = q; single.sinks = {sink};
            auto expected = QueryEngine(g).run(single);
            auto found = all.foundAt.find(sink);
            EXPECT_EQ(found != all.foundAt.end(), expected.found());
            if (found != all.foundAt.end()) {
              EXPECT_EQ(found->second.edges, expected.edges);
              EXPECT_EQ(found->second.witnessComplete, expected.witnessComplete);
            }
          }
        }
      }
      q.maxProductStates = 1;
      auto partial = QueryEngine(g).runToSinks(q);
      EXPECT_FALSE(partial.complete);
      EXPECT_NE(partial.status, QueryStatus::NotFound);
      q.requireNonEmpty = false;
      partial = QueryEngine(g).runToSinks(q);
      EXPECT_FALSE(partial.complete);
      EXPECT_EQ(partial.status, QueryStatus::Found);
      ASSERT_EQ(partial.foundAt.size(), 1u);
      EXPECT_TRUE(partial.foundAt.count(source));
    }
  }
}
TEST(UseTraceSSAObjects, RandomDifferential) {
  std::mt19937 rng(919827);
  for (unsigned trial = 0; trial < 250; ++trial) {
    SCOPED_TRACE(trial);
    TraceFlowGraph g;
    auto objects = [&]() {
      if (rng()%6 == 0) return ObjectSet::unknown();
      std::vector<ObjectID> v;
      for (ObjectID o = 1; o <= 4; ++o) if (rng()%2) v.push_back(o);
      return ObjectSet::known(v);
    };
    for (unsigned i = 0; i < 7; ++i) {
      auto n = node(g);
      for (unsigned j = 0; j < rng()%4; ++j)
        g.annotate(n, rng()%2 ? Event::Release : Event::Allocate, objects(),
                    rng()%2 ? Certainty::Must : Certainty::May);
    }
    for (ID a = 0; a < 7; ++a) for (ID b = 0; b < 7; ++b) if (rng()%6 == 0) {
      auto kind = rng()%3;
      edge(g,a,b,objects(), kind == 0 ? FlowKind::History :
                            kind == 1 ? FlowKind::Call : FlowKind::Return,
                            kind == 0 ? NoNativeID : rng()%3);
    }
    for (auto context : {ContextMode::Insensitive, ContextMode::Balanced, ContextMode::Realizable}) {
      auto q = query(0,6); q.sources.push_back(1); q.sinks.push_back(5);
      q.context = context; q.requireNonEmpty = rng()%2;
      if (rng()%2) q.contextLimit = rng()%4;
      if (rng()%2) q.automaton = Automaton::doubleFree();
      if (rng()%3 == 0) q.trapEvents = Event::Allocate;
      if (rng()%9 == 0) g.addIssue("incomplete random fixture");
      compare(g,q);
    }
  }
}
TEST(UseTraceSSAObjects, IncompleteAndBudgets) {
  TraceFlowGraph g; auto a = node(g), b = node(g), c = node(g);
  edge(g,a,b,ObjectSet::known({1}),FlowKind::Thread);
  auto q = query(a,c);
  auto r = compare(g,q); EXPECT_EQ(r.status(1),QueryStatus::Unknown);
  EXPECT_EQ(r.status(2),QueryStatus::NotFound);
  edge(g,a,c,ObjectSet::known({2}));
  g.addIssue("missing effects"); r = compare(g,q);
  EXPECT_EQ(r.status(2),QueryStatus::Found); EXPECT_EQ(r.status(3),QueryStatus::Unknown);
  q.maxProductStates = 1; compare(g,q);
}
TEST(UseTraceSSAObjects, LegacyDifferential) {
  static const unsigned legacy[] = {
#include "LegacyResults.inc"
  };
  std::mt19937 rng(18272);
  for (unsigned trial = 0; trial < 150; ++trial) {
    SCOPED_TRACE(trial);
    Program p; auto entry = p.addBlock("entry"), left = p.addBlock("left"),
      right = p.addBlock("right"), join = p.addBlock("join");
    p.addEdge(entry,left); p.addEdge(entry,right); p.addEdge(left,join); p.addEdge(right,join);
    if (trial%2) p.addEdge(join,left);
    std::vector<TemporalEffect> effects;
    for (unsigned b = 0; b < 4; ++b) for (unsigned i = 0; i < 3; ++i) {
      auto site = p.addOperation(b,"site");
      for (unsigned j = 0; j < 2; ++j) {
        std::vector<ObjectID> ids;
        for (ObjectID o = 1; o <= 3; ++o) if (rng()%2) ids.push_back(o);
        auto objects = rng()%5 ? ObjectSet::known(ids) : ObjectSet::unknown();
        Event e = rng()%3 == 0 ? Event::Allocate : rng()%2 ? Event::Release : Event::Dereference;
        auto certainty = rng()%2 ? Certainty::May : Certainty::Must;
        effects.push_back({site,objects,e,certainty});
      }
    }
    TraceFlowGraph shared;
    TemporalHistory::append(shared,0,"shared",p,effects);
    unsigned legacyBits = 0, legacyBit = 0;
    for (bool df : {false,true}) {
      auto nq = df ? queries::doubleFree(shared) : queries::useAfterFree(shared);
      auto batch = compare(shared,nq,{1,2,3,UnknownResource});
      for (auto object : {ObjectID(1),ObjectID(2),ObjectID(3),UnknownResource}) {
        auto expected = (legacy[trial] & (1u << legacyBit)) ?
                         QueryStatus::Found : QueryStatus::NotFound;
        EXPECT_EQ(expected,batch.status(object));
        if (expected == QueryStatus::Found) legacyBits |= 1u << legacyBit;
        ++legacyBit;
      }
    }
    EXPECT_EQ(legacyBits, legacy[trial]);
  }
}
TEST(UseTraceSSAObjects, ImportEffectGuardsAreAuthoritative) {
  FunctionLayout f; f.id=0; auto b=f.control.addBlock("entry");
  auto a=f.control.addOperation(b,"allocate"), r=f.control.addOperation(b,"release");
  LocatedSVFGNode source; source.id=1; source.function=0; source.definitionSite=a;
  source.definitionEffects={{Event::Allocate,ObjectSet::known({7}),Certainty::May}};
  LocatedSVFGNode target; target.id=2; target.function=0; target.definitionSite=r;
  LocatedSVFGEdge e; e.id=1; e.from=1; e.to=2; e.objects=ObjectSet::known({1,2});
  e.useEffects={{Event::Release,ObjectSet::known({2,3}),Certainty::May},
                {Event::Dereference,ObjectSet::unknown(),Certainty::Must}};
  SVFGConstructionInput input; input.functions={f}; input.nodes={source,target}; input.edges={e};
  auto imported=SVFGImporter::build(input);
  const auto &definition=imported.graph.node(imported.native.nodes.at(1));
  EXPECT_EQ(definition.effects[0].objects.objects(),std::vector<ObjectID>({7}));
  const auto &use=imported.graph.node(imported.graph.edge(imported.native.edges.at(1)).from);
  ASSERT_EQ(use.effects.size(),2u);
  EXPECT_EQ(use.effects[0].objects.objects(),std::vector<ObjectID>({2,3}));
  EXPECT_TRUE(use.effects[1].objects.isUnknown());
  EXPECT_EQ(imported.graph.effectiveEvent(use.id,2).certainty,Certainty::May);
  auto q=query(definition.id,use.id); q.automaton=Automaton::ordered({Event::Allocate,Event::Release});
  EXPECT_EQ(compare(imported.graph,q,{1,2,3,7}).found.count(),0u);
  input.edges[0].boundary=true;
  EXPECT_THROW(SVFGImporter::build(input),std::invalid_argument);
}
TEST(UseTraceSSAObjects, ModelsAndUnknownCandidate) {
  TraceFlowGraph g; auto a=node(g), b=node(g);
  edge(g,a,b,ObjectSet::known({1,2}));
  CallPorts call; call.callee="free"; call.arguments.resize(1);
  call.arguments[0].resourceEffects={{a,ObjectSet::unknown(),Certainty::Must}};
  LibraryModels().apply(g,call);
  g.annotate(b,Event::Dereference,ObjectSet::unknown());
  EXPECT_EQ(g.effectiveEvent(a,1).certainty,Certainty::May);
  EXPECT_EQ(g.resourceCandidates(),std::vector<ObjectID>({1,2,UnknownResource}));
  auto scan=DefectDetector(g).scan(DefectKind::UseAfterFree);
  ASSERT_EQ(scan.findings.size(),1u);
  EXPECT_EQ(scan.findings[0].objects,std::vector<ObjectID>({1,2}));
  EXPECT_TRUE(scan.findings[0].witnessObject.has_value());
  EXPECT_EQ(compare(g,queries::useAfterFree(g)).found.count(),2u);
  call.callee="malloc"; call.allocationEffects={{b,ObjectSet::known({1}),Certainty::May}};
  LibraryModels().apply(g,call);
  EXPECT_EQ(g.node(b).effects.size(),2u);
  EXPECT_EQ(g.node(b).events,Event::None);
}
TEST(UseTraceSSAObjects, DetectorRequiresTargetEventAtSink) {
  TraceFlowGraph g;
  auto release = node(g), firstUse = node(g), otherUse = node(g);
  edge(g, release, firstUse);
  edge(g, firstUse, otherUse);
  g.annotate(release, Event::Release, ObjectSet::known({1}));
  g.annotate(firstUse, Event::Dereference, ObjectSet::known({1}));
  g.annotate(otherUse, Event::Dereference, ObjectSet::known({2}));
  auto scan = DefectDetector(g).scan(DefectKind::UseAfterFree);
  ASSERT_EQ(scan.findings.size(), 1u);
  EXPECT_EQ(scan.findings.front().objects, std::vector<ObjectID>({1}));
  EXPECT_EQ(scan.findings.front().result.nodes.back(), firstUse);
}
TEST(UseTraceSSAObjects, DetectorGroupsWitnessObjectsAndKeepsSinkOrder) {
  TraceFlowGraph g;
  auto release = node(g), secondObject = node(g), firstObject = node(g), later = node(g);
  g.annotate(release, Event::Release, ObjectSet::known({1, 2}));
  g.annotate(secondObject, Event::Dereference, ObjectSet::known({2}));
  g.annotate(firstObject, Event::Dereference, ObjectSet::known({1}));
  g.annotate(later, Event::Dereference, ObjectSet::known({1, 2}));
  edge(g, release, secondObject);
  edge(g, release, firstObject);
  edge(g, firstObject, later);
  for (auto limit : {std::optional<std::size_t>{}, std::optional<std::size_t>{3}}) {
    DefectDetector detector(g, limit);
    auto scan = detector.scan(DefectKind::UseAfterFree);
    ASSERT_EQ(scan.findings.size(), 3u);
    EXPECT_EQ(scan.findings[0].result.nodes.back(), secondObject);
    EXPECT_EQ(scan.findings[1].result.nodes.back(), firstObject);
    EXPECT_EQ(scan.findings[2].result.nodes.back(), later);
    for (const auto &finding : scan.findings) {
      auto q = queries::useAfterFree(g);
      q.contextLimit = limit;
      q.memoryObject = finding.witnessObject;
      q.sinks = {finding.result.nodes.back()};
      EXPECT_EQ(finding.result.edges, QueryEngine(g).run(q).edges);
    }
    auto first = detector.run(DefectKind::UseAfterFree);
    EXPECT_EQ(first.result.nodes, scan.findings.front().result.nodes);
    EXPECT_EQ(first.objects, scan.findings.front().objects);
    EXPECT_EQ(first.witnessObject, scan.findings.front().witnessObject);
  }
}
TEST(UseTraceSSAObjects, SerializationAndVerification) {
  TraceFlowGraph g; auto a=node(g);
  g.annotate(a,Event::Release,ObjectSet::unknown());
  g.annotate(a,Event::Allocate,ObjectSet::known({}),Certainty::May);
  g.annotate(a,Event::Dereference,ObjectSet::known({1,2}));
  EXPECT_TRUE(g.verify());
  std::ostringstream json,dot; g.printJSON(json); g.printDOT(dot);
  EXPECT_NE(json.str().find("\"objects\":null"),std::string::npos);
  EXPECT_NE(json.str().find("\"objects\":[]"),std::string::npos);
  EXPECT_EQ(json.str().find("\"object\":"),std::string::npos);
  EXPECT_NE(dot.str().find("@null"),std::string::npos);
  EXPECT_NE(dot.str().find("@[]"),std::string::npos);
  EXPECT_THROW(g.annotate(a,Event::None,ObjectSet::unknown()),std::invalid_argument);
  FlowNode bad; bad.effects={{Event::Release,ObjectSet::unknown(),static_cast<Certainty>(9)}};
  g.addNode(bad); EXPECT_FALSE(g.verify());
}
TEST(UseTraceSSAObjects, PropertyUniversePreservesTopAndInvalidatesOnMutation) {
  TraceFlowGraph g;
  auto release = node(g), use = node(g);
  g.annotate(release, Event::Release, ObjectSet::known({1}));
  g.annotate(use, Event::Dereference, ObjectSet::unknown());
  edge(g, release, use);
  g.setResourceUniverse({1, 1});
  EXPECT_EQ(g.resourceCandidates(), std::vector<ObjectID>({1}));
  EXPECT_TRUE(g.node(use).effects.front().objects.isUnknown());
  std::ostringstream json;
  g.printJSON(json);
  EXPECT_NE(json.str().find("\"resource_universe\":[\"1\"]"), std::string::npos);
  g.annotate(release, Event::Release, ObjectSet::known({2}));
  EXPECT_EQ(g.resourceCandidates(), std::vector<ObjectID>({1, 2, UnknownResource}));
}
TEST(UseTraceSSAObjects, WitnessProvenance) {
  Program p; auto b=p.addBlock("entry"), a=p.addOperation(b,"free"), c=p.addOperation(b,"use");
  TraceFlowGraph g;
  auto h=TemporalHistory::append(g,0,"provenance",p,
      {{a,ObjectSet::known({1,2}),Event::Release,Certainty::Must,101},
       {c,ObjectSet::known({2,3}),Event::Dereference,Certainty::Must,202}});
  auto q=queries::useAfterFree(g); auto batch=compare(g,q);
  EXPECT_EQ(batch.status(2),QueryStatus::Found);
  q.memoryObject=2; auto fixed=QueryEngine(g).run(q);
  ASSERT_TRUE(fixed.found());
  EXPECT_EQ(fixed.nodes.front(),h.after(g,a)); EXPECT_EQ(fixed.nodes.back(),h.after(g,c));
  EXPECT_EQ(g.node(fixed.nodes.front()).native,101u);
  EXPECT_EQ(g.node(fixed.nodes.back()).native,202u);
  EXPECT_EQ(fixed.automatonStates,std::vector<ID>({1,2}));
}
} // namespace
