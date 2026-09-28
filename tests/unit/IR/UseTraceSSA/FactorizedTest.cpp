#include "IR/UseTraceSSA/Models.h"
#include "IR/UseTraceSSA/DefectDetector.h"
#include <sstream>
#include "IR/UseTraceSSA/SVFGImporter.h"
#include "IR/UseTraceSSA/TemporalHistory.h"
#include <gtest/gtest.h>
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
ObjectBatchResult compare(const TraceFlowGraph &g, Query q,
                           std::vector<ObjectID> objects = {1, 2, 3, 4, UnknownResource}) {
  q.memoryObject.reset();
  auto batch = QueryEngine(g).runObjects({q, ObjectUniverse(objects)});
  for (auto object : objects) {
    q.memoryObject = object;
    auto fixed = QueryEngine(g).run(q);
    EXPECT_EQ(batch.status(object), fixed.status) << "object=" << object;
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
