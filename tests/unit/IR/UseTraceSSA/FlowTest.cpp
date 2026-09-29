#include "IR/UseTraceSSA/DefectDetector.h"
#include "IR/UseTraceSSA/Models.h"
#include "IR/UseTraceSSA/TemporalHistory.h"
#include "IR/UseTraceSSA/SVFGImporter.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace lotus::usetracessa;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
template <typename F> void throws(F f) {
  bool caught = false; try { f(); } catch (const std::exception &) { caught = true; } CHECK(caught);
}
FlowNodeID node(TraceFlowGraph &g, std::string label, Event event = Event::None,
                Certainty certainty = Certainty::Must) {
  FlowNode n; n.label = std::move(label); n.events = event; n.certainty = certainty;
  return g.addNode(n);
}
FlowEdgeID edge(TraceFlowGraph &g, ID a, ID b, FlowKind kind = FlowKind::Direct,
                CallSiteID call = NoNativeID, ObjectSet objects = ObjectSet::unknown()) {
  FlowEdge e; e.from = a; e.to = b; e.kind = kind; e.callSite = call;
  e.objects = std::move(objects); return g.addEdge(e);
}
Query query(ID a, ID b) { Query q; q.sources = {a}; q.sinks = {b}; return q; }
void witness(const TraceFlowGraph &g, const QueryResult &r, ContextMode mode) {
  CHECK(r.status == QueryStatus::Found); CHECK(r.witnessComplete);
  CHECK(r.nodes.size() == r.edges.size() + 1);
  CHECK(r.automatonStates.size() == r.nodes.size());
  std::vector<CallSiteID> stack;
  for (std::size_t i = 0; i < r.edges.size(); ++i) {
    const auto &e = g.edge(r.edges[i]);
    CHECK(e.from == r.nodes[i]); CHECK(e.to == r.nodes[i+1]);
    if (mode == ContextMode::Insensitive) continue;
    if (e.kind == FlowKind::Call) stack.push_back(e.callSite);
    else if (e.kind == FlowKind::Return) {
      if (stack.empty()) CHECK(mode == ContextMode::Realizable);
      else { CHECK(stack.back() == e.callSite); stack.pop_back(); }
    }
  }
  if (mode == ContextMode::Balanced) CHECK(stack.empty());
}
void traps() {
  TraceFlowGraph g; auto a = node(g,"source"), b = node(g,"trap"), c = node(g,"sink");
  edge(g,a,b); edge(g,b,c);
  QueryEngine engine(g); auto q = query(a,c);
  CHECK(engine.run(q).found());
  q.traps = {b}; CHECK(engine.run(q).status == QueryStatus::NotFound);
  CHECK(engine.allPathsHitTraps(q).status == CoverageStatus::AllPathsTrapped);
  q.traps = {a}; CHECK(!engine.run(q).found());
  q.traps = {c}; CHECK(!engine.run(q).found());
  q = query(c,a); CHECK(engine.allPathsHitTraps(q).status == CoverageStatus::SinkUnreachable);
}
void bypass() {
  TraceFlowGraph g; auto a=node(g,"source"), b=node(g,"check",Event::Sanitize),
    c=node(g,"unchecked"), d=node(g,"sink");
  edge(g,a,b); edge(g,b,d); edge(g,a,c); edge(g,c,d);
  auto q=query(a,d); q.trapEvents=Event::Sanitize;
  auto r=QueryEngine(g).run(q); CHECK(r.found()); CHECK(r.nodes[1]==c);
  CHECK(QueryEngine(g).allPathsHitTraps(q).status==CoverageStatus::UntrappedPath);
  g.annotate(c,Event::Sanitize); CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
void weakTrap() {
  TraceFlowGraph g; auto a=node(g,"source"), b=node(g,"may sanitize",Event::Sanitize,Certainty::May),
    c=node(g,"sink"); edge(g,a,b); edge(g,b,c);
  auto q=query(a,c); q.trapEvents=Event::Sanitize; CHECK(QueryEngine(g).run(q).found());
  q.traps={b}; CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
void guardSets() {
  CHECK(ObjectSet::unknown().contains(42)); CHECK(!ObjectSet::known({}).contains(42));
  CHECK(!ObjectSet::unknown().intersects(ObjectSet::known({})));
  CHECK(ObjectSet::known({1,2}).intersects(ObjectSet::known({2,3})));
  CHECK(!ObjectSet::known({1}).intersects(ObjectSet::known({3})));
  TraceFlowGraph g; auto a=node(g,"a"), b=node(g,"b"), c=node(g,"c");
  edge(g,a,b,FlowKind::Memory,NoNativeID,ObjectSet::known({1}));
  edge(g,b,c,FlowKind::Memory,NoNativeID,ObjectSet::known({2}));
  auto q=query(a,c); CHECK(QueryEngine(g).run(q).found()); // do NOT globally intersect
  q.memoryObject=1; CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
  edge(g,a,c,FlowKind::Memory,NoNativeID,ObjectSet::known({}));
  CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
  edge(g,a,c,FlowKind::Memory); CHECK(QueryEngine(g).run(q).found());
}
struct ImportFixture {
  TraceFlowGraph g;
  Program p;
  ID x,y,check,consume;
  SVFGSnapshot snapshot;
  ImportFixture() {
    auto b=p.addBlock("entry"); x=p.addValue("x"); y=p.addValue("y");
    p.addOperation(b,"input",{}, {x}); check=p.addOperation(b,"sanitizer",{x});
    consume=p.addOperation(b,"y=copy(x)",{x},{y});
    g.addHistory(0,"f",Graph::build(p));
    g.annotate(g.after(0,check,x),Event::Sanitize);
    snapshot.nodes={{100,Port::definition(0,x)},{200,Port::definition(0,y)}};
    SVFGEdgeRecord e; e.id=900; e.from=100; e.to=200;
    e.consumption=Port::afterUse(0,consume,x); e.nativeKind=123;
    e.objects=ObjectSet::known({17,23}); snapshot.edges={e};
  }
};
void svfgOrdering() {
  ImportFixture f; auto imported=SVFGImporter::append(f.g,f.snapshot);
  CHECK(imported.nodes.at(100)==f.g.definition(0,f.x));
  auto q=query(f.g.definition(0,f.x),f.g.definition(0,f.y));
  CHECK(QueryEngine(f.g).run(q).found());
  q.trapEvents=Event::Sanitize; CHECK(QueryEngine(f.g).run(q).status==QueryStatus::NotFound);
  const auto &e=f.g.edge(imported.edges.at(900));
  CHECK(e.from==f.g.after(0,f.consume,f.x)); CHECK(e.nativeKind==123);
  CHECK(e.objects.objects()==std::vector<ObjectID>({17,23}));
  CHECK(f.g.verify());
}
void invalidImport() {
  ImportFixture f; auto size=f.g.edges().size();
  f.snapshot.edges[0].consumption.reset();
  throws([&]{SVFGImporter::append(f.g,f.snapshot);}); CHECK(f.g.edges().size()==size);
  f.snapshot.edges[0].consumption=Port::definition(0,f.x);
  throws([&]{SVFGImporter::append(f.g,f.snapshot);}); CHECK(f.g.edges().size()==size);
  f.snapshot.edges[0].consumption=Port::afterUse(0,f.consume,f.x);
  f.snapshot.edges[0].from=200;
  throws([&]{SVFGImporter::append(f.g,f.snapshot);}); CHECK(f.g.edges().size()==size);
  f.snapshot.edges[0].from=100; f.snapshot.nodes.push_back(f.snapshot.nodes[0]);
  throws([&]{SVFGImporter::append(f.g,f.snapshot);}); CHECK(f.g.edges().size()==size);
}
void nativeContract() {
  struct NativeEdge { SVFGEdgeRecord record; };
  struct NativeNode {
    SVFGNodeRecord record; std::vector<NativeEdge*> edges;
    auto OutEdgeBegin() const { return edges.begin(); }
    auto OutEdgeEnd() const { return edges.end(); }
  };
  ImportFixture f; NativeEdge e{f.snapshot.edges[0]};
  NativeNode a{f.snapshot.nodes[0],{&e}},b{f.snapshot.nodes[1],{}};
  std::map<NativeID,NativeNode*> native={{100,&a},{200,&b}};
  auto r=SVFGImporter::appendNative(f.g,native,
      [](const NativeNode &n){return n.record;},[](const NativeEdge &e){return e.record;});
  CHECK(r.nodes.size()==2); CHECK(r.edges.size()==1);
  CHECK(f.g.edge(r.edges.at(900)).native==900);
}
Query resourceQuery(const TraceFlowGraph &g, bool doubleFree) {
  auto q = doubleFree ? queries::doubleFree(g) : queries::useAfterFree(g);
  auto objects = g.resourceCandidates();
  if (!objects.empty()) q.memoryObject = objects.front();
  return q;
}
void paperResources() {
  Program p; auto entry=p.addBlock("entry"), nonnull=p.addBlock("nonnull"), join=p.addBlock("join");
  auto alloc=p.addOperation(entry,"malloc"), check=p.addOperation(entry,"compare null");
  auto nullEdge=p.addEdge(entry,join,"null"), goodEdge=p.addEdge(entry,nonnull,"nonnull");
  (void)nullEdge;
  auto guard=p.addEdgeOperation(goodEdge,"assume nonnull");
  auto read=p.addOperation(nonnull,"*p"), first=p.addOperation(nonnull,"free(p)");
  p.addEdge(nonnull,join); auto second=p.addOperation(join,"free(q), q aliases p");
  ObjectSet obj=ObjectSet::known({41});
  std::vector<TemporalEffect> accesses={{alloc,obj,Event::Allocate,Certainty::Must},
    {check,obj,Event::None,Certainty::Must},{guard,obj,Event::NonNull,Certainty::Must},
    {read,obj,Event::Dereference,Certainty::Must},{first,obj,Event::Release,Certainty::Must},
    {second,obj,Event::Release,Certainty::Must}};
  TraceFlowGraph g; auto h=TemporalHistory::append(g,0,"paper",p,accesses);
  auto df=QueryEngine(g).run(resourceQuery(g,true)); CHECK(df.found());
  witness(g,df,ContextMode::Realizable);
  auto safe=queries::uncheckedUse({h.after(g,alloc)},{h.after(g,read)}); safe.memoryObject=41;
  CHECK(QueryEngine(g).allPathsHitTraps(safe).status==CoverageStatus::AllPathsTrapped);
  auto path=query(h.after(g,first),h.after(g,second)); CHECK(QueryEngine(g).run(path).found());
  CHECK(!QueryEngine(g).run(resourceQuery(g,false)).found());
}
void noAliasUnion() {
  Program p; auto b=p.addBlock("b"); auto x=p.addOperation(b,"free through {1,2}");
  auto y=p.addOperation(b,"free through {2,3}"); TraceFlowGraph g;
  auto h=TemporalHistory::append(g,0,"aliases",p,
    {{x,ObjectSet::known({1,2}),Event::Release,Certainty::May},
     {y,ObjectSet::known({2,3}),Event::Release,Certainty::May}});
  auto q=query(h.after(g,x),h.after(g,y)); q.automaton=Automaton::doubleFree();
  q.memoryObject=1;
  CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
  q.memoryObject=3; CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
  q=query(h.after(g,x),h.after(g,y)); q.automaton=Automaton::doubleFree();
  q.memoryObject=2;
  CHECK(QueryEngine(g).run(q).found());
}
void unknownAlias() {
  Program p; auto b=p.addBlock("b"); auto x=p.addOperation(b,"free unknown");
  auto y=p.addOperation(b,"free known"); TraceFlowGraph g;
  auto h=TemporalHistory::append(g,0,"unknown",p,
    {{x,ObjectSet::unknown(),Event::Release,Certainty::May},
     {y,ObjectSet::known({2}),Event::Release,Certainty::Must}});
  auto q=query(h.after(g,x),h.after(g,y)); q.automaton=Automaton::doubleFree();
  q.memoryObject=2;
  CHECK(QueryEngine(g).run(q).found()); auto candidates = g.resourceCandidates();
  CHECK(std::find(candidates.begin(), candidates.end(), UnknownResource) != candidates.end());
}
void resourceLoop(bool reset) {
  Program p; auto e=p.addBlock("entry"), b=p.addBlock("loop"), x=p.addBlock("exit");
  p.addEdge(e,b); p.addEdge(b,b); p.addEdge(b,x);
  auto alloc=p.addOperation(reset?b:e,"allocate"); auto free=p.addOperation(b,"free");
  TraceFlowGraph g; auto obj=ObjectSet::known({1});
  TemporalHistory::append(g,0,"loop",p,
    {{alloc,obj,Event::Allocate,Certainty::Must},{free,obj,Event::Release,Certainty::Must}});
  auto q=resourceQuery(g,true); auto r=QueryEngine(g).run(q);
  CHECK(r.found()!=reset); if(r.found()) witness(g,r,ContextMode::Realizable);
}
void useAfterFree() {
  Program p; auto b=p.addBlock("b"); auto a=p.addOperation(b,"alloc"),f=p.addOperation(b,"free"),
    u=p.addOperation(b,"read"); TraceFlowGraph g; auto o=ObjectSet::known({77});
  TemporalHistory::append(g,0,"uaf",p,{{a,o,Event::Allocate,Certainty::Must},
    {f,o,Event::Release,Certainty::Must},{u,o,Event::Dereference,Certainty::Must}});
  CHECK(QueryEngine(g).run(resourceQuery(g,false)).found());
  CHECK(QueryEngine(g).run(resourceQuery(g,true)).status==QueryStatus::NotFound);
}
void unallocatedResource() {
  Program p;
  auto block = p.addBlock("entry");
  auto first = p.addOperation(block, "free(formal p)");
  auto read = p.addOperation(block, "*formal alias q");
  auto second = p.addOperation(block, "free(formal alias q)");
  auto object = ObjectSet::known({93});
  TraceFlowGraph g;
  TemporalHistory::append(g, 0, "borrowed-object", p,
      {{first, object, Event::Release, Certainty::Must},
       {read, object, Event::Dereference, Certainty::Must},
       {second, object, Event::Release, Certainty::Must}});
  CHECK(g.select(Event::Allocate).empty());
  auto df = QueryEngine(g).run(resourceQuery(g,true));
  CHECK(df.found());
  witness(g, df, ContextMode::Realizable);
  CHECK(QueryEngine(g).run(resourceQuery(g,false)).found());

  TraceFlowGraph onlyOne;
  TemporalHistory::append(onlyOne, 0, "single-release", p,
      {{first, object, Event::Release, Certainty::Must}});
  CHECK(QueryEngine(onlyOne).run(resourceQuery(onlyOne,true)).status == QueryStatus::NotFound);
}
void matchingCalls() {
  TraceFlowGraph g; auto a=node(g,"source"),b=node(g,"callee"),c=node(g,"wrong caller sink");
  edge(g,a,b,FlowKind::Call,1); edge(g,b,c,FlowKind::Return,2);
  auto q=query(a,c); CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
  q.context=ContextMode::Balanced; CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
  q.context=ContextMode::Insensitive; CHECK(QueryEngine(g).run(q).found());
  q.context=ContextMode::Realizable; edge(g,b,c,FlowKind::Return,1);
  witness(g,QueryEngine(g).run(q),q.context);
}
void realizableSegments() {
  TraceFlowGraph g; auto a=node(g,"inside callee"),b=node(g,"caller"),c=node(g,"new callee");
  edge(g,a,b,FlowKind::Return,55); edge(g,b,c,FlowKind::Call,66);
  auto q=query(a,c); witness(g,QueryEngine(g).run(q),q.context);
  q.context=ContextMode::Balanced; CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
void nestedCalls() {
  TraceFlowGraph g; std::vector<FlowNodeID> v; const unsigned depth=35;
  for(unsigned i=0;i<2*depth+1;++i) v.push_back(node(g,std::to_string(i)));
  for(unsigned i=0;i<depth;++i) edge(g,v[i],v[i+1],FlowKind::Call,i+1);
  for(unsigned i=0;i<depth;++i) edge(g,v[depth+i],v[depth+i+1],FlowKind::Return,depth-i);
  auto q=query(v.front(),v.back()); q.context=ContextMode::Balanced;
  auto r=QueryEngine(g).run(q); witness(g,r,q.context); CHECK(r.edges.size()==2*depth);
}
void balancedCycle() {
  TraceFlowGraph g; auto a=node(g,"caller"),b=node(g,"callee");
  edge(g,a,b,FlowKind::Call,9); edge(g,b,a,FlowKind::Return,9);
  auto q=query(a,a); q.context=ContextMode::Balanced; q.requireNonEmpty=true;
  auto r=QueryEngine(g).run(q); witness(g,r,q.context); CHECK(r.edges.size()==2);
}
void recursion() {
  TraceFlowGraph g; auto a=node(g,"caller"),b=node(g,"entry"),c=node(g,"exit"),d=node(g,"sink");
  edge(g,a,b,FlowKind::Call,1); edge(g,b,b,FlowKind::Call,2);
  edge(g,b,c); edge(g,c,c,FlowKind::Return,2); edge(g,c,d,FlowKind::Return,1);
  auto q=query(a,d); q.context=ContextMode::Balanced;
  witness(g,QueryEngine(g).run(q),q.context);
  // Both sources and sinks inside recursive functions are supported.
  q=query(c,b); CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
void resourceCall() {
  Program caller; auto b=caller.addBlock("entry"); auto alloc=caller.addOperation(b,"alloc");
  auto call=caller.addOperation(b,"callee"); auto free=caller.addOperation(b,"free");
  Program callee; auto cb=callee.addBlock("entry"); auto cf=callee.addOperation(cb,"free");
  auto o=ObjectSet::known({1}); TraceFlowGraph g;
  auto h=TemporalHistory::append(g,0,"caller",caller,
    {{alloc,o,Event::Allocate,Certainty::Must},{call,o,Event::None,Certainty::Must},
     {free,o,Event::Release,Certainty::Must}});
  auto t=TemporalHistory::append(g,1,"callee",callee,
    {{cf,o,Event::Release,Certainty::Must}});
  TemporalHistory::connectCall(g,h,call,9,{t});
  auto q=resourceQuery(g,true); q.context=ContextMode::Balanced;
  auto r=QueryEngine(g).run(q); witness(g,r,q.context);
  CHECK(std::find(r.nodes.begin(),r.nodes.end(),t.after(g,cf))!=r.nodes.end());
}
void calleeTrap() {
  Program p; auto b=p.addBlock("caller"); auto a=p.addOperation(b,"source"),
    call=p.addOperation(b,"call"),s=p.addOperation(b,"sink");
  Program c; auto cb=c.addBlock("callee"); auto check=c.addOperation(cb,"sanitize");
  auto o=ObjectSet::known({1}); TraceFlowGraph g;
  auto h=TemporalHistory::append(g,0,"caller",p,
    {{a,o,Event::Source,Certainty::Must},{call,o,Event::None,Certainty::Must},
     {s,o,Event::Sink,Certainty::Must}});
  auto t=TemporalHistory::append(g,1,"callee",c,
    {{check,o,Event::Sanitize,Certainty::Must}});
  TemporalHistory::connectCall(g,h,call,7,{t});
  CHECK(QueryEngine(g).allPathsHitTraps([&] { auto q=queries::taint(g); q.memoryObject=1; return q; }()).status==CoverageStatus::AllPathsTrapped);
}
void limits() {
  TraceFlowGraph g; auto a=node(g,"a"),b=node(g,"b"),c=node(g,"c"); edge(g,a,b);edge(g,b,c);
  auto q=query(a,c); q.maxProductStates=1; CHECK(QueryEngine(g).run(q).status==QueryStatus::Unknown);
  q.maxProductStates=100; q.maxWork=1; CHECK(QueryEngine(g).run(q).status==QueryStatus::Unknown);
  q.maxWork=100; q.maxWitnessEdges=1; auto r=QueryEngine(g).run(q);
  CHECK(r.found()); CHECK(!r.witnessComplete);
  TraceFlowGraph h; a=node(h,"call");b=node(h,"callee");c=node(h,"return");
  edge(h,a,b,FlowKind::Call,1);edge(h,b,c,FlowKind::Return,1);
  q=query(a,c);q.context=ContextMode::Balanced;q.maxSummaryPairs=1;
  CHECK(QueryEngine(h).run(q).status==QueryStatus::Unknown);
}
void threads() {
  TraceFlowGraph g; auto a=node(g,"thread1"),b=node(g,"thread2");edge(g,a,b,FlowKind::Thread);
  auto q=query(a,b);CHECK(QueryEngine(g).run(q).status==QueryStatus::Unknown);
  q.includeThreadEdges=true;CHECK(QueryEngine(g).run(q).found());
}
void orderedEvents() {
  TraceFlowGraph g;auto a=node(g,"source"),b=node(g,"alloc",Event::Allocate),
    c=node(g,"release",Event::Release);edge(g,a,b);edge(g,b,c);
  auto q=query(a,c);q.automaton=Automaton::ordered({Event::Allocate,Event::Release});
  CHECK(QueryEngine(g).run(q).found());
  q.automaton=Automaton::ordered({Event::Release,Event::Allocate});
  CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
void noAddressTaint() {
  TraceFlowGraph g;auto addr=node(g,"buffer address"),out=node(g,"contents after read");
  CallPorts c;c.callee="recv";c.arguments.resize(3);c.arguments[1].value={addr};
  c.arguments[1].memoryOut={out};LibraryModels().apply(g,c);
  CHECK(g.node(addr).events==Event::None);CHECK(hasEvent(g.node(out).events,Event::Source));
  CHECK(QueryEngine(g).run(query(addr,out)).status==QueryStatus::NotFound);
}
void unknownLibrary() {
  TraceFlowGraph g;auto a=node(g,"input"),b=node(g,"output"),c=node(g,"unrelated");
  CallPorts call;call.callee="unknown";call.arguments.resize(1);
  call.arguments[0].value={a};call.returnValue={b};LibraryModels().apply(g,call);
  CHECK(!g.complete());CHECK(QueryEngine(g).run(query(a,b)).found());
  CHECK(QueryEngine(g).run(query(a,c)).status==QueryStatus::Unknown);
}
void missingMemoryPorts() {
  TraceFlowGraph g;auto a=node(g,"address only");CallPorts c;c.callee="read";c.arguments.resize(3);
  c.arguments[1].value={a};LibraryModels().apply(g,c);CHECK(!g.complete());
  CHECK(g.select(Event::Source).empty());CHECK(!g.issues().empty());
}
void customModel() {
  TraceFlowGraph g;auto a=node(g,"input"),b=node(g,"checked"),c=node(g,"sink");edge(g,a,b);edge(g,b,c);
  LibraryModels m;m.registerModel("my_check",[](TraceFlowGraph &g,const CallPorts &c){
    for(auto n:c.arguments.at(0).value)g.annotate(n,Event::Sanitize);
  });
  CallPorts call;call.callee="my_check";call.arguments.resize(1);call.arguments[0].value={b};m.apply(g,call);
  auto q=query(a,c);q.trapEvents=Event::Sanitize;CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
void noMemsetKill() {
  TraceFlowGraph g;auto a=node(g,"byte"),b=node(g,"region after partial memset");
  CallPorts c;c.callee="llvm.memset.p0i8.i64";c.arguments.resize(3);
  c.arguments[0].memoryOut={b};c.arguments[1].value={a};LibraryModels().apply(g,c);
  CHECK(!hasEvent(g.node(b).events,Event::Sanitize));CHECK(QueryEngine(g).run(query(a,b)).found());
}
void slices() {
  TraceFlowGraph g;auto a=node(g,"a"),b=node(g,"b"),c=node(g,"c"),d=node(g,"d");
  edge(g,a,b);edge(g,b,c);edge(g,d,c);QueryEngine e(g);
  CHECK(e.slice({a})==std::vector<FlowNodeID>({a,b,c}));
  CHECK(e.slice({c},true)==std::vector<FlowNodeID>({a,b,c,d}));
  CHECK(e.slice({a},false,{b})==std::vector<FlowNodeID>({a}));
  auto results=e.runBatch({query(a,c),query(c,a)});CHECK(results[0].found());
  CHECK(results[1].status==QueryStatus::NotFound);
}
void serialization() {
  TraceFlowGraph g;auto a=node(g,"quote\" backslash\\ newline\n");auto b=node(g,"b");
  g.annotate(a,Event::Release,ObjectSet::known({18446744073709551610ULL}));edge(g,a,b,FlowKind::Memory,NoNativeID,ObjectSet::known({1,2}));
  std::ostringstream x,y;g.printJSON(x);g.printJSON(y);CHECK(x.str()==y.str());
  CHECK(x.str().find("18446744073709551610")!=std::string::npos);
  CHECK(x.str().find("\\\"")!=std::string::npos);CHECK(g.verify());
  g.printDOT(y);CHECK(y.str().find("digraph UseTraceSSA")!=std::string::npos);
}
void invalidQueries() {
  TraceFlowGraph g;auto a=node(g,"a");auto q=query(a,a);
  q.automaton.states=0;throws([&]{QueryEngine(g).run(q);});q.automaton.states=1;
  q.automaton.transition=[](ID,Event){return 99;};throws([&]{QueryEngine(g).run(q);});
  throws([&]{edge(g,a,99);});throws([&]{edge(g,a,a,FlowKind::Call);});
  q=query(a,a);q.sources={99};throws([&]{QueryEngine(g).run(q);});
}
void multiSource() {
  TraceFlowGraph g;auto a=node(g,"a"),b=node(g,"b"),c=node(g,"c"),d=node(g,"d");edge(g,b,d);
  auto q=query(a,c);q.sources={a,b};q.sinks={c,d};auto r=QueryEngine(g).run(q);
  CHECK(r.found());CHECK(r.nodes.front()==b);CHECK(r.nodes.back()==d);
  q.traps={b};CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
void randomDyck() {
  std::mt19937 rng(0x81926);
  for(unsigned trial=0;trial<200;++trial) {
    TraceFlowGraph g;const ID n=8;for(ID i=0;i<n;++i)node(g,std::to_string(i));
    for(ID i=0;i<n;++i)for(ID j=i+1;j<n;++j)if(rng()%4==0){
      unsigned k=rng()%3;edge(g,i,j,k==0?FlowKind::Direct:k==1?FlowKind::Call:FlowKind::Return,
                             k==0?NoNativeID:1+rng()%2);
    }
    for(auto mode:{ContextMode::Balanced,ContextMode::Realizable}){
      bool expected=false;
      std::function<void(ID,std::vector<CallSiteID>)> dfs=[&](ID v,std::vector<CallSiteID> stack){
        if(v==n-1 && (mode==ContextMode::Realizable || stack.empty()))expected=true;
        for(auto eid:g.outgoing(v)){
          auto copy=stack;const auto &e=g.edge(eid);
          if(e.kind==FlowKind::Call)copy.push_back(e.callSite);
          if(e.kind==FlowKind::Return){
            if(copy.empty()){if(mode==ContextMode::Balanced)continue;}
            else {if(copy.back()!=e.callSite)continue;copy.pop_back();}
          }
          dfs(e.to,std::move(copy));
        }
      };
      dfs(0,{});auto q=query(0,n-1);q.context=mode;auto r=QueryEngine(g).run(q);
      CHECK(r.status!=QueryStatus::Unknown);CHECK(r.found()==expected);
      if(r.found())witness(g,r,mode);
    }
  }
}

void autoBuilder() {
  FunctionLayout f;f.id=0;f.name="f";auto b=f.control.addBlock("entry");
  auto a=f.control.addOperation(b,"source"), check=f.control.addOperation(b,"check"),
    use=f.control.addOperation(b,"copy");
  SVFGConstructionInput input;input.functions={f};
  input.nodes={{10,0,a,"source",Event::Source,Certainty::Must},
               {11,0,check,"check result",Event::None,Certainty::Must},
               {12,0,use,"copy result",Event::Sink,Certainty::Must}};
  LocatedSVFGEdge e;e.id=1;e.from=10;e.to=11;e.useEvents=Event::Sanitize;
  LocatedSVFGEdge e2;e2.id=2;e2.from=10;e2.to=12;e2.objects=ObjectSet::known({41});
  input.edges={e,e2};auto r=SVFGImporter::build(input);
  CHECK(r.graph.verify());CHECK(r.native.nodes.size()==3);CHECK(r.native.edges.size()==2);
  auto q=queries::taint(r.graph);CHECK(QueryEngine(r.graph).run(q).status==QueryStatus::NotFound);
  q.trapEvents=Event::None;CHECK(QueryEngine(r.graph).run(q).found());
  CHECK(r.graph.node(r.native.nodes.at(10)).native==10);
  CHECK(r.graph.edge(r.native.edges.at(2)).objects.contains(41));
}
void autoMemoryPhi() {
  FunctionLayout f;f.id=0;f.name="memory";
  auto a=f.control.addBlock("a"),b=f.control.addBlock("b"),c=f.control.addBlock("c"),
    d=f.control.addBlock("d");
  f.control.addEdge(a,b);f.control.addEdge(a,c);
  auto left=f.control.addEdge(b,d),right=f.control.addEdge(c,d);
  auto ldef=f.control.addOperation(b,"storechi-left"),rdef=f.control.addOperation(c,"storechi-right"),
    phi=f.control.addOperation(d,"memory phi"),load=f.control.addOperation(d,"loadmu");
  auto lu=f.control.addEdgeOperation(left,"phi-left"),ru=f.control.addEdgeOperation(right,"phi-right");
  SVFGConstructionInput in;in.functions={f};
  in.nodes={{10,0,ldef,"region.1",Event::Source,Certainty::Must},
            {11,0,rdef,"region.2",Event::None,Certainty::Must},
            {12,0,phi,"region.3",Event::None,Certainty::Must},
            {13,0,load,"load",Event::Sink,Certainty::Must}};
  LocatedSVFGEdge e;e.id=1;e.from=10;e.to=12;e.consumerSite=lu;e.kind=FlowKind::Memory;
  auto e2=e;e2.id=2;e2.from=11;e2.consumerSite=ru;
  auto e3=e;e3.id=3;e3.from=12;e3.to=13;e3.consumerSite=load;
  in.edges={e,e2,e3};auto r=SVFGImporter::build(in);
  CHECK(QueryEngine(r.graph).run(queries::taint(r.graph)).found());
  CHECK(!QueryEngine(r.graph).run(query(r.native.nodes.at(10),r.native.nodes.at(11))).found());
  in.edges[0].consumerSite=phi;throws([&]{SVFGImporter::build(in);});
}
void autoCall() {
  FunctionLayout caller;caller.id=0;caller.name="caller";
  auto b=caller.control.addBlock("entry"),s=caller.control.addOperation(b,"actual-in"),
    r=caller.control.addOperation(b,"actual-out");
  FunctionLayout callee;callee.id=1;callee.name="callee";
  auto c=callee.control.addBlock("entry"),ret=callee.control.addOperation(c,"formal-ret");
  SVFGConstructionInput in;in.functions={caller,callee};
  in.nodes={{1,0,s,"actual-in",Event::Source,Certainty::Must},
            {2,0,r,"actual-out",Event::Sink,Certainty::Must},
            {3,1,InvalidID,"formal-in",Event::None,Certainty::Must},
            {4,1,ret,"formal-ret",Event::None,Certainty::Must}};
  LocatedSVFGEdge call;call.id=1;call.from=1;call.to=3;call.kind=FlowKind::Call;
  call.callSite=88;call.boundary=true;
  LocatedSVFGEdge local;local.id=2;local.from=3;local.to=4;
  auto back=call;back.id=3;back.from=4;back.to=2;back.kind=FlowKind::Return;
  in.edges={call,local,back};auto h=SVFGImporter::build(in);
  auto q=queries::taint(h.graph);q.context=ContextMode::Balanced;
  witness(h.graph,QueryEngine(h.graph).run(q),q.context);
}
void defectDetector() {
  TraceFlowGraph graph;
  auto first = node(graph, "first free", Event::Release);
  auto second = node(graph, "second free", Event::Release);
  graph.setNative(first, 10);
  graph.setNative(second, 20);
  edge(graph, first, second, FlowKind::History);
  auto found = DefectDetector(graph).run(DefectKind::DoubleFree);
  CHECK(found.result.status == QueryStatus::Found);
  CHECK(found.nativeWitness == std::vector<NativeID>({10, 20}));
  CHECK(DefectDetector(graph).run(DefectKind::UseAfterFree).result.status ==
        QueryStatus::Unknown);
  TraceFlowGraph empty;
  CHECK(DefectDetector(empty).run(DefectKind::DoubleFree).result.status ==
        QueryStatus::Unknown);
  TraceFlowGraph incomplete;
  node(incomplete, "release", Event::Release);
  incomplete.addIssue("unmodeled call");
  CHECK(DefectDetector(incomplete).run(DefectKind::DoubleFree).result.status ==
        QueryStatus::Unknown);

  TraceFlowGraph resource;
  auto release = node(resource, "free", Event::Release);
  auto reset = node(resource, "fresh allocation", Event::Allocate);
  auto later = node(resource, "second free", Event::Release);

  edge(resource, release, reset, FlowKind::History);
  edge(resource, reset, later, FlowKind::History);
  auto result = DefectDetector(resource).run(DefectKind::DoubleFree);
  CHECK(!result.witnessObject); // No finding needs a concrete object witness.
  CHECK(result.result.status ==
        QueryEngine(resource).run(queries::doubleFree(resource)).status);
  CHECK(result.result.status == QueryStatus::NotFound);

  std::mt19937 rng(78);
  for (unsigned trial = 0; trial < 80; ++trial) {
    TraceFlowGraph graph;
    for (unsigned i = 0; i < 18; ++i) {
      unsigned choice = rng() % 5;
      Event event = choice == 0 ? Event::Release :
                    choice == 1 ? Event::Dereference :
                    choice == 2 ? Event::Allocate : Event::None;
      auto id = node(graph, "resource", event,
                     choice == 2 && (rng() % 2) ? Certainty::Must : Certainty::May);
      (void)id;
    }
    for (FlowNodeID i = 0; i < graph.nodes().size(); ++i)
      for (FlowNodeID j = i + 1; j < graph.nodes().size(); ++j)
        if (rng() % 7 == 0) edge(graph, i, j, FlowKind::History);
    for (auto kind : {DefectKind::DoubleFree, DefectKind::UseAfterFree}) {
      Query query = kind == DefectKind::DoubleFree ? queries::doubleFree(graph) :
                                                     queries::useAfterFree(graph);
      auto report = DefectDetector(graph).run(kind);
      if (graph.select(Event::Release).empty() || query.sinks.empty())
        CHECK(report.result.status == QueryStatus::Unknown);
      else
        CHECK(report.result.status == QueryEngine(graph).run(query).status);
      auto scan = DefectDetector(graph).scan(kind);
      std::size_t referenceCount = 0;
      auto sinks = query.sinks;
      for (auto sink : sinks) {
        query.sinks = {sink};
        if (QueryEngine(graph).run(query).found()) ++referenceCount;
      }
      CHECK(scan.findings.size() == referenceCount);
    }
  }
}
int main(int argc,char **argv){
  const std::map<std::string,std::function<void()>> tests={
    {"traps",traps},{"bypass",bypass},{"weak-trap",weakTrap},{"object-guards",guardSets},
    {"svfg-ordering",svfgOrdering},{"invalid-import",invalidImport},{"native-contract",nativeContract},
    {"paper-resources",paperResources},{"no-alias-union",noAliasUnion},{"unknown-alias",unknownAlias},
    {"loop-reset",[]{resourceLoop(true);}},{"loop-double-free",[]{resourceLoop(false);}},
    {"use-after-free",useAfterFree},{"unallocated-resource",unallocatedResource},{"matching-calls",matchingCalls},{"realizable-segments",realizableSegments},
    {"nested-calls",nestedCalls},{"balanced-cycle",balancedCycle},{"recursion",recursion},
    {"resource-call",resourceCall},{"callee-trap",calleeTrap},{"limits",limits},{"threads",threads},
    {"ordered-events",orderedEvents},
    {"no-address-taint",noAddressTaint},{"unknown-library",unknownLibrary},{"missing-memory-ports",missingMemoryPorts},
    {"custom-model",customModel},{"no-memset-kill",noMemsetKill},{"slices",slices},
    {"serialization",serialization},{"invalid-queries",invalidQueries},{"multi-source",multiSource},
    {"random-dyck",randomDyck},{"auto-builder",autoBuilder},
    {"auto-memory-phi",autoMemoryPhi},{"auto-call",autoCall},
    {"defect-detector",defectDetector}};
  try{
    if(argc==2){auto it=tests.find(argv[1]);if(it==tests.end())throw std::runtime_error("unknown test");it->second();}
    else for(const auto &t:tests)t.second();
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
  return 0;
}
