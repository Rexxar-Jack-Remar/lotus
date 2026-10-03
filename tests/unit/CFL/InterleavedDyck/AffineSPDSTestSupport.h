#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Synchronized.h"
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>

namespace lotus::cfl::interleaved_dyck::affine::test {
inline void require(bool yes, const char *condition, int line) {
  if (!yes) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + condition);
}
#define AFF_CHECK(condition) ::lotus::cfl::interleaved_dyck::affine::test::require((condition), #condition, __LINE__)
template <class Exception = std::exception, class F> void throws(F f) {
  bool caught = false; try { f(); } catch (const Exception &) { caught = true; }
  AFF_CHECK(caught);
}
inline Matrix numbered(std::size_t n, std::uint64_t value) {
  Matrix m(n);
  for (std::size_t i = 0; i < n*n && i < 64; ++i) if ((value >> i) & 1U) m.set(i/n,i%n);
  return m;
}
inline Matrix randomMatrix(std::size_t n, std::mt19937 &rng) {
  Matrix m(n);
  for (std::size_t i=0;i<n;++i) for (std::size_t j=0;j<n;++j) m.set(i,j,(rng()&1U)!=0);
  return m;
}
inline AffineSpace randomSpace(std::size_t n, std::mt19937 &rng) {
  AffineSpace result(n); const auto count=rng()%5;
  for (unsigned i=0;i<count;++i) result.addPoint(randomMatrix(n,rng));
  return result;
}
inline std::vector<Matrix> enumerate(const AffineSpace &space) {
  std::vector<Matrix> result;
  if (space.empty()) return result;
  AFF_CHECK(space.rank() < 15);
  for (std::size_t mask=0;mask<(std::size_t{1}<<space.rank());++mask) {
    auto point=space.offset();
    for (std::size_t i=0;i<space.rank();++i) if ((mask>>i)&1U) point^=space.directions()[i];
    result.push_back(space.decodeEntries(point));
  }
  return result;
}
inline std::uint32_t mask(const AffineSpace &space) {
  std::uint32_t result=0;
  for(unsigned i=0;i<16;++i) if(space.contains(numbered(2,i))) result |= std::uint32_t{1}<<i;
  return result;
}
inline Label label(unsigned index) {
  switch(index) {
  case 0: return Label::neutral();
  case 1: return Label::openParenthesis(0);
  case 2: return Label::closeParenthesis(0);
  case 3: return Label::openBracket(0);
  case 4: return Label::closeBracket(0);
  case 5: return Label::openParenthesis(1);
  case 6: return Label::closeParenthesis(1);
  case 7: return Label::openBracket(1);
  default: return Label::closeBracket(1);
  }
}
inline bool step(std::vector<unsigned> &stack, Label l, bool call) {
  const auto open=call?LabelKind::OpenParenthesis:LabelKind::OpenBracket;
  const auto close=call?LabelKind::CloseParenthesis:LabelKind::CloseBracket;
  if(l.kind==open) stack.insert(stack.begin(),l.id);
  else if(l.kind==close) {
    if(stack.empty() || stack.front()!=l.id) return false;
    stack.erase(stack.begin());
  }
  return true;
}
inline void matrixArithmetic() {
  std::mt19937 rng(13);
  for (std::size_t n : {1U,2U,3U,8U,9U,17U,63U,64U,65U}) {
    auto a=randomMatrix(n,rng), b=randomMatrix(n,rng); Matrix expected(n);
    for(std::size_t i=0;i<n;++i) for(std::size_t j=0;j<n;++j) {
      bool bit=false; for(std::size_t k=0;k<n;++k) bit ^= a.get(i,k) && b.get(k,j);
      expected.set(i,j,bit);
    }
    AFF_CHECK(a*b==expected); AFF_CHECK(a*Matrix::identity(n)==a);
    AFF_CHECK(Matrix::parse(a.str())==a); AFF_CHECK((a^a).isZero());
    const auto c=randomMatrix(n,rng); AFF_CHECK((a*b)*c==a*(b*c));
  }
  BitVector a(130),b(130); a.set(0);a.set(64);a.set(129); b.set(129);
  AFF_CHECK(a.dot(b)); AFF_CHECK((a^b).firstSet()==0);
  auto copied=a;copied.set(0,false);
  AFF_CHECK(a.test(0));AFF_CHECK(!copied.test(0));
  BitVector assigned(1);assigned=a;assigned.set(64,false);
  AFF_CHECK(a.test(64));
  throws<std::out_of_range>([&]{a.set(130);});
  throws<std::invalid_argument>([]{(void)Matrix(0);});
  throws<std::invalid_argument>([]{(void)Matrix(std::numeric_limits<std::size_t>::max());});
  for (const auto &text : {"", "10/1", "12/01", "1/1", "10-01"})
    throws<std::invalid_argument>([&]{(void)Matrix::parse(text);});
  throws<std::out_of_range>([]{(void)Matrix(2).block(2,1);});
  throws<std::invalid_argument>([]{(void)(Matrix(2)*Matrix(3));});
}
inline void canonicalAffine() {
  std::mt19937 rng(24);
  for(unsigned t=0;t<200;++t) {
    std::vector<Matrix> points; for(unsigned j=0;j<6;++j) points.push_back(randomMatrix(3,rng));
    AffineSpace a(3),b(3);
    for(const auto &m:points) a.addPoint(m);
    std::shuffle(points.begin(),points.end(),rng);
    for(const auto &m:points) b.addPoint(m);
    AFF_CHECK(a==b); AFF_CHECK(a.contains(b)); AFF_CHECK(!a.joinWith(a));
    for(const auto &m:points) AFF_CHECK(a.contains(m));
  }
  auto z=AffineSpace::singleton(Matrix(2)); AFF_CHECK(!z.empty()); AFF_CHECK(z.rank()==0);
  auto changed=z;changed.addPoint(Matrix::identity(2));
  AFF_CHECK(z.rank()==0);AFF_CHECK(changed.rank()==1);AFF_CHECK(z!=changed);
  AFF_CHECK(z!=AffineSpace(2)); AFF_CHECK(AffineSpace::top(2).rank()==4);
}
inline void exhaustiveSeparation() {
  std::map<std::uint32_t,AffineSpace> spaces; spaces.emplace(0,AffineSpace(2));
  std::vector<AffineSpace> queue{AffineSpace(2)};
  for(std::size_t at=0;at<queue.size();++at) {
    const auto current=queue[at];
    for(unsigned i=0;i<16;++i) {
      auto next=current; next.addPoint(numbered(2,i)); auto key=mask(next);
      if(spaces.emplace(key,next).second) queue.push_back(std::move(next));
    }
  }
  AFF_CHECK(spaces.size()==308);
  for(const auto &a:spaces) for(const auto &b:spaces) {
    AFF_CHECK(a.second.intersects(b.second)==((a.first&b.first)!=0));
    auto cert=separate(a.second,b.second);
    AFF_CHECK(static_cast<bool>(cert)==(a.first && b.first && !(a.first&b.first)));
    if(cert) {
      AFF_CHECK(cert->verify(a.second,b.second));
      auto bad=*cert; bad.left_value=!bad.left_value; AFF_CHECK(!bad.verify(a.second,b.second));
    }
  }
  std::cout<<"  exhaustive affine-space pairs: "<<spaces.size()*spaces.size()<<'\n';
}
inline void semiringLaws() {
  std::mt19937 rng(52);
  for(unsigned t=0;t<600;++t) {
    const std::size_t n=1+rng()%3; AffineSemiring d(n);
    auto a=randomSpace(n,rng),b=randomSpace(n,rng),c=randomSpace(n,rng);
    AFF_CHECK(d.combine(a,b)==d.combine(b,a)); AFF_CHECK(d.combine(a,a)==a);
    auto inplace=a;const auto combined=d.combine(a,b);
    AFF_CHECK(d.combineWith(inplace,b)==(combined!=a));AFF_CHECK(inplace==combined);
    AFF_CHECK(d.combine(d.combine(a,b),c)==d.combine(a,d.combine(b,c)));
    AFF_CHECK(d.extend(d.extend(a,b),c)==d.extend(a,d.extend(b,c)));
    AFF_CHECK(d.extend(a,d.combine(b,c))==d.combine(d.extend(a,b),d.extend(a,c)));
    AFF_CHECK(d.extend(d.combine(a,b),c)==d.combine(d.extend(a,c),d.extend(b,c)));
    AFF_CHECK(d.extend(a,d.one())==a); AFF_CHECK(d.extend(d.one(),a)==a);
    AFF_CHECK(d.extend(a,d.zero())==d.zero());
    auto fused=c;const auto fused_expected=d.combine(c,d.extend(a,b));
    AFF_CHECK(d.extendAndCombine(fused,a,b)==(fused_expected!=c));
    AFF_CHECK(fused==fused_expected);
    auto aliased=a;const auto aliased_expected=d.combine(a,d.extend(a,b));
    d.extendAndCombine(aliased,aliased,b);AFF_CHECK(aliased==aliased_expected);
    AffineSpace exact(n);
    for(const auto &x:enumerate(a)) for(const auto &y:enumerate(b)) exact.addPoint(x*y);
    AFF_CHECK(exact==a.product(b));

    if (!a.empty() && !b.empty()) {
      auto grown=a;AffineDelta input;
      grown.joinWithDelta(c,&input);
      auto incremental=a.product(b),expected=incremental;
      AffineDelta output;
      d.extendDeltaAndCombine(incremental,grown,b,input,true,&output);
      d.extendAndCombine(expected,grown,b);
      AFF_CHECK(incremental==expected);
      if (!incremental.empty() && !c.empty()) {
        auto downstream=a.product(b).product(c);
        auto downstream_expected=incremental.product(c);
        d.extendDeltaAndCombine(downstream,incremental,c,output,true,nullptr);
        AFF_CHECK(downstream==downstream_expected);
      }

      grown=b;input.clear();grown.joinWithDelta(c,&input);
      incremental=a.product(b);expected=incremental;
      output.clear();
      d.extendDeltaAndCombine(incremental,a,grown,input,false,&output);
      d.extendAndCombine(expected,a,grown);
      AFF_CHECK(incremental==expected);

      const auto fixed=d.lift(randomMatrix(n,rng));
      const auto prepared=d.prepareWeight(fixed);
      grown=a;input.clear();grown.joinWithDelta(c,&input);
      incremental=a.product(fixed);expected=incremental;
      output.clear();
      d.extendPreparedInputDeltaAndCombine(
          incremental,grown,fixed,prepared,input,&output);
      d.extendAndCombinePrepared(expected,grown,fixed,prepared);
      AFF_CHECK(incremental==expected);

      if (!c.empty()) {
        grown=b;input.clear();grown.joinWithDelta(fixed,&input);
        incremental=d.extend(a,d.extend(b,c));
        expected=d.extend(a,d.extend(grown,c));output.clear();
        d.extendPushDeltaAndCombine(
            incremental,a,grown,c,input,true,&output);
        AFF_CHECK(incremental==expected);

        grown=c;input.clear();grown.joinWithDelta(fixed,&input);
        incremental=d.extend(a,d.extend(b,c));
        expected=d.extend(a,d.extend(b,grown));output.clear();
        d.extendPushDeltaAndCombine(
            incremental,a,b,grown,input,false,&output);
        AFF_CHECK(incremental==expected);
      }
    }
  }
  // Explicit UV term: offsets are zero; only product of directions is nonzero.
  AffineSpace a=AffineSpace::singleton(Matrix(2)),b=a;
  a.addPoint(Matrix::parse("01/00")); b.addPoint(Matrix::parse("00/10"));
  AFF_CHECK(a.product(b).contains(Matrix::parse("10/00")));
  AffineSemiring prepared_domain(2);
  const auto singleton =
      prepared_domain.lift(Matrix::parse("11/01"));
  const auto prepared = prepared_domain.prepareWeight(singleton);
  AFF_CHECK(static_cast<bool>(prepared));
  AFF_CHECK(prepared_domain.extendPrepared(a,singleton,prepared)==
            prepared_domain.extend(a,singleton));
  auto fused=a;auto expected=prepared_domain.combine(
      a,prepared_domain.extend(b,singleton));
  prepared_domain.extendAndCombinePrepared(fused,b,singleton,prepared);
  AFF_CHECK(fused==expected);
  AFF_CHECK(!prepared_domain.prepareWeight(prepared_domain.one()));
  throws<std::invalid_argument>([]{AffineSemiring(2).combine(AffineSpace(2),AffineSpace(3));});
}
inline void observerConstruction() {
  Edge a{0,1,label(0)},b{1,2,label(0)}; Graph g;g.addEdge(a.source,a.target,a.label);g.addEdge(b.source,b.target,b.label);
  auto p=HistoryObserver::parity({a}); AFF_CHECK(p.trace({a,a})==Matrix::identity(2));
  auto order=HistoryObserver::orderedPair({a},{b});
  AFF_CHECK(order.trace({a,b})!=order.trace({b,a}));
  AFF_CHECK(order.trace({a,b}).get(0,2)); AFF_CHECK(!order.trace({b,a}).get(0,2));
  auto same=HistoryObserver::orderedPair({a},{a});
  AFF_CHECK(!same.trace({a}).get(0,2)); AFF_CHECK(same.trace({a,a}).get(0,2));
  auto cycle=HistoryObserver::cyclic({a},3);
  AFF_CHECK(cycle.trace({a,a,a})==Matrix::identity(3));
  AFF_CHECK(cycle.trace({a,a})!=Matrix::identity(3));
  auto sum=HistoryObserver::directSum({p,order,cycle}); AFF_CHECK(sum.dimension()==8);sum.validate(g);
  AFF_CHECK(sum.trace({a,b,a}).block(2,3)==order.trace({a,b,a}));
  std::stringstream text;sum.write(text);auto parsed=HistoryObserver::read(text);
  AFF_CHECK(parsed.blocks()==sum.blocks()); AFF_CHECK(parsed.trace({a,b,a})==sum.trace({a,b,a}));
  for(const auto &bad : {"", "dimension -1\n", "dimension 0\n", "dimension 2\nblock 1 2\n",
      "dimension 2\nblock 0 1\n", "dimension 1\nedge 0 1 normal 1\nedge 0 1 normal 1\n",
      "dimension 2\nedge 0 1 normal 1\n", "dimension 2\nunknown 2\n"}) {
    std::stringstream input(bad);throws<std::invalid_argument>([&]{(void)HistoryObserver::read(input);});
  }
  HistoryObserver outside(2);outside.set({7,8,label(0)},Matrix(2));
  throws<std::invalid_argument>([&]{outside.validate(g);});
}
struct CorrelationGraph { Graph graph; Edge x,y; Vertex target=14; };
inline CorrelationGraph correlationGraph() {
  CorrelationGraph r;
  // First branch: (a b [f [g or (b (a [g [f. Second branch:
  // )b )a ]f ]g or )a )b ]g ]f. Calls need x=y; fields x!=y.
  auto path=[&](const std::vector<Vertex>&vertices,const std::vector<Label>&labels){
    for(std::size_t i=0;i<labels.size();++i) r.graph.addEdge(vertices[i],vertices[i+1],labels[i]);
  };
  path({0,1,2,3,7},{label(1),label(5),label(3),label(7)});
  path({0,4,5,6,7},{label(5),label(1),label(7),label(3)});
  path({7,8,9,10,14},{label(6),label(2),label(4),label(8)});
  path({7,11,12,13,14},{label(2),label(6),label(8),label(4)});
  r.x={0,4,label(5)};r.y={7,11,label(2)};return r;
}
inline void jointCorrelation() {
  auto fixture=correlationGraph();const auto &g=fixture.graph;
  auto observer=HistoryObserver::directSum({HistoryObserver::parity({fixture.x}),HistoryObserver::parity({fixture.y})});
  AFF_CHECK(spds::Solver().prepare(g).queryFrom(0).mayReach(14));
  auto analysis=Solver().prepare(g,observer);
  auto result=analysis.queryFrom(0);const auto &c=result.compare(14);
  AFF_CHECK(!analysis.analyzeFrom(0).mayReach(0,14));
  AFF_CHECK(!analysis.analyzeTo(14).mayReach(0,14));
  AFF_CHECK(c.spdsMayReach());AFF_CHECK(c.independentMayReach(observer));AFF_CHECK(!c.mayReach());
  AFF_CHECK(c.verdict()==Verdict::AffineSeparated);AFF_CHECK(c.certificate()->verify(c.callHistory(),c.fieldHistory()));
  auto reverse=analysis.queryTo(14);AFF_CHECK(!reverse.mayReach(0));
  AFF_CHECK(reverse.compare(0).callHistory()==c.callHistory());
  AFF_CHECK(reverse.compare(0).fieldHistory()==c.fieldHistory());
  AFF_CHECK(!Solver().prepare(g).queryFrom(0).mayReach(14));
  auto identity=Solver().prepare(g,HistoryObserver()).queryFrom(0);
  AFF_CHECK(identity.mayReach(14));
  AFF_CHECK(analysis.analyzeAll(ComparisonMode::Projection).mayReach(0,14));
  AFF_CHECK(analysis.analyzeAll(ComparisonMode::Independent).mayReach(0,14));
  AFF_CHECK(!analysis.analyzeAll(ComparisonMode::Joint).mayReach(0,14));
  const std::vector<Pair> demand{{0,14}};
  AFF_CHECK(analysis.analyzeDemands(demand,ComparisonMode::Projection).mayReach(0,14));
  AFF_CHECK(analysis.analyzeDemands(demand,ComparisonMode::Independent).mayReach(0,14));
  AFF_CHECK(!analysis.analyzeDemands(demand,ComparisonMode::Joint).mayReach(0,14));
  std::vector<Pair> all_targets;
  for(Vertex target:g.vertices())all_targets.push_back({0,target});
  auto batch=analysis.analyzeDemands(all_targets,ComparisonMode::Joint);
  for(Vertex target:g.vertices())
    AFF_CHECK(batch.mayReach(0,target)==result.mayReach(target));
}
inline void orderSensitivity() {
  Graph g;
  for(unsigned i=0;i<4;++i) g.addEdge(i,i+1,label(std::vector<unsigned>{1,2,3,8}[i]));
  g.addEdge(0,5,label(1));g.addEdge(5,6,label(6));g.addEdge(6,7,label(3));g.addEdge(7,4,label(4));
  std::vector<Edge> a{{0,1,label(1)},{5,6,label(6)}},b{{1,2,label(2)},{0,5,label(1)}};
  auto counts=HistoryObserver::directSum({HistoryObserver::parity(a),HistoryObserver::parity(b)});
  AFF_CHECK(Solver().prepare(g,counts).queryFrom(0).mayReach(4));
  auto order=HistoryObserver::orderedPair(a,b);
  auto analysis=Solver().prepare(g,order);
  auto forward=analysis.queryFrom(0),backward=analysis.queryTo(4);
  AFF_CHECK(!forward.mayReach(4));AFF_CHECK(!backward.mayReach(0));
  AFF_CHECK(forward.compare(4).callHistory()==backward.compare(0).callHistory());
  AFF_CHECK(forward.compare(4).fieldHistory()==backward.compare(0).fieldHistory());
}
inline void crossingAndNeutral() {
  Graph g;g.addEdge(-4,10,Label::openParenthesis(std::numeric_limits<unsigned>::max()));
  g.addEdge(10,20,label(3));g.addEdge(20,30,Label::closeParenthesis(std::numeric_limits<unsigned>::max()));
  g.addEdge(30,40,label(4));g.addEdge(40,50,label(0));g.addVertex(-99);
  HistoryObserver o(3);std::mt19937 rng(914);
  for(const auto &e:g.edges())o.set(e,randomMatrix(3,rng));
  auto q=Solver().prepare(g,o).queryFrom(-4);
  AFF_CHECK(q.mayReach(50));AFF_CHECK(q.compare(50).callHistory()==AffineSpace::singleton(o.trace(g.edges())));
  AFF_CHECK(q.mayAccept(20,{std::numeric_limits<unsigned>::max()},{0}));
  AFF_CHECK(!q.mayAccept(20,{0},{0}));AFF_CHECK(!q.mayReach(999));AFF_CHECK(!q.mayReach(-99));
  AFF_CHECK(Solver().prepare(g,o).queryFrom(-99).mayReach(-99));
  // A zero event matrix is a valid concrete history, not an unreachable weight.
  o.set(g.edges()[0],Matrix(3));auto zero=Solver().prepare(g,o).queryFrom(-4);
  AFF_CHECK(zero.mayReach(50));AFF_CHECK(zero.compare(50).callHistory()==AffineSpace::singleton(Matrix(3)));
}
inline void exhaustiveWords() {
  std::size_t checked=0;
  for(unsigned length=0;length<=4;++length) {
    unsigned total=1;for(unsigned i=0;i<length;++i)total*=9;
    for(unsigned code=0;code<total;++code) {
      Graph g;g.addVertex(0);unsigned value=code;bool call=true,field=true;
      std::vector<unsigned> cs,fs;HistoryObserver o(2);std::vector<Edge> trace;
      for(unsigned i=0;i<length;++i) {
        Label l=label(value%9);value/=9;Edge e{i,i+1,l};g.addEdge(e.source,e.target,e.label);trace.push_back(e);
        o.set(e,numbered(2,(code+3*i)%16));
        if(call)call=step(cs,l,true);
        if(field)field=step(fs,l,false);
      }
      auto analysis=Solver().prepare(g,o);
      auto q=analysis.queryFrom(0);const auto &c=q.compare(length);
      AFF_CHECK(!c.callHistory().empty()==(call&&cs.empty()));
      AFF_CHECK(!c.fieldHistory().empty()==(field&&fs.empty()));
      AFF_CHECK(c.mayReach()==(call&&field&&cs.empty()&&fs.empty()));
      if(call&&field)AFF_CHECK(q.compareStacks(length,cs,fs).mayReach());
      auto b=analysis.queryTo(length);
      AFF_CHECK(b.compare(0).callHistory()==c.callHistory());
      AFF_CHECK(b.compare(0).fieldHistory()==c.fieldHistory());++checked;
    }
  }
  AFF_CHECK(checked==7381);std::cout<<"  exhaustive words: "<<checked<<'\n';
}
using Stack = std::vector<unsigned>;
using StackHulls = std::map<Stack,AffineSpace>;
struct Oracle { std::vector<StackHulls> calls,fields;std::set<Vertex> concrete; };
inline Oracle dagOracle(const Graph &g, Vertex source, const HistoryObserver &o) {
  Oracle result;result.calls.resize(g.vertices().size());result.fields.resize(g.vertices().size());
  auto join=[&](StackHulls &map,const Stack &s,const Matrix &m){auto it=map.find(s);if(it==map.end())map.emplace(s,AffineSpace::singleton(m));else it->second.addPoint(m);};
  std::function<void(Vertex,Stack,Stack,bool,bool,Matrix)> visit;
  visit=[&](Vertex node,Stack cs,Stack fs,bool cv,bool fv,Matrix value){
    if(cv)join(result.calls.at(node),cs,value);
    if(fv)join(result.fields.at(node),fs,value);
    if(cv&&fv&&cs.empty()&&fs.empty())result.concrete.insert(node);
    for(const auto &e:g.edges())if(e.source==node){
      auto a=cs,b=fs;bool ca=cv&&step(a,e.label,true),fb=fv&&step(b,e.label,false);
      visit(e.target,a,b,ca,fb,value*o.matrix(e));
    }
  };
  visit(source,{}, {},true,true,Matrix::identity(o.dimension()));return result;
}
inline AffineSpace lookup(const StackHulls &h,const Stack &s,std::size_t dim) {
  auto it=h.find(s);return it==h.end()?AffineSpace(dim):it->second;
}
inline AffineSpace unionHulls(const StackHulls &h,std::size_t dim) {
  AffineSpace result(dim);for(const auto &entry:h)result.joinWith(entry.second);return result;
}
inline void randomDAGs() {
  std::mt19937 rng(2340);std::size_t queries=0;
  for(unsigned trial=0;trial<120;++trial) {
    Graph g;for(unsigned v=0;v<5;++v)g.addVertex(v);
    for(unsigned a=0;a<5;++a)for(unsigned b=a+1;b<5;++b)if(rng()%3==0){
      g.addEdge(a,b,label(rng()%9));if(rng()%4==0)g.addEdge(a,b,label(rng()%9));
    }
    HistoryObserver o(2+trial%2);for(const auto &e:g.edges())o.set(e,randomMatrix(o.dimension(),rng));
    auto analysis=Solver().prepare(g,o);
    std::vector<QueryResult> backwards;
    for(unsigned t=0;t<5;++t)backwards.push_back(analysis.queryTo(t));
    for(unsigned s=0;s<5;++s) {
      auto oracle=dagOracle(g,s,o);auto q=analysis.queryFrom(s);
      Options any;any.parentheses=any.brackets=spds::StackAcceptance::Any;
      auto prefix=Solver(any).prepare(g,o).queryFrom(s);
      for(unsigned t=0;t<5;++t) {
        const auto &r=q.compare(t);auto c=lookup(oracle.calls[t],{},o.dimension()),f=lookup(oracle.fields[t],{},o.dimension());
        AFF_CHECK(r.callHistory()==c);AFF_CHECK(r.fieldHistory()==f);
        AFF_CHECK(backwards[t].compare(s).callHistory()==c);AFF_CHECK(backwards[t].compare(s).fieldHistory()==f);
        AFF_CHECK(!oracle.concrete.count(t)||r.mayReach());
        AFF_CHECK(prefix.compare(t).callHistory()==unionHulls(oracle.calls[t],o.dimension()));
        AFF_CHECK(prefix.compare(t).fieldHistory()==unionHulls(oracle.fields[t],o.dimension()));
        for(const auto &cp:oracle.calls[t])for(const auto &fp:oracle.fields[t]){
          auto exact=q.compareStacks(t,cp.first,fp.first);
          AFF_CHECK(exact.callHistory()==cp.second);AFF_CHECK(exact.fieldHistory()==fp.second);
        }
        ++queries;
      }
    }
  }
  std::cout<<"  exact DAG endpoint queries: "<<queries<<'\n';AFF_CHECK(queries==3000);
}
// Independent finite image oracle: enumerate the 16 possible 2x2 history
// matrices inside an exact single-stack CFL closure. Handles unbounded cycles.
inline unsigned scalarProduct(unsigned a,unsigned b) {
  unsigned c=0;
  for(unsigned i=0;i<2;++i)for(unsigned j=0;j<2;++j){
    unsigned x=0;for(unsigned k=0;k<2;++k)x^=((a>>(2*i+k))&1U)&((b>>(2*k+j))&1U);
    c|=x<<(2*i+j);
  }
  return c;
}
using ImageTable=std::vector<std::vector<std::set<unsigned>>>;
inline ImageTable cyclicOracle(const Graph &g,const std::map<Edge,unsigned,EdgeLess>&weights,bool call) {
  const auto n=g.vertices().size();ImageTable table(n,std::vector<std::set<unsigned>>(n));
  const auto open=call?LabelKind::OpenParenthesis:LabelKind::OpenBracket;
  const auto close=call?LabelKind::CloseParenthesis:LabelKind::CloseBracket;
  for(std::size_t i=0;i<n;++i)table[i][i].insert(9); // identity
  for(const auto &e:g.edges())if(e.label.kind!=open&&e.label.kind!=close)table[e.source][e.target].insert(weights.at(e));
  bool changed=true;
  while(changed){
    changed=false;
    for(std::size_t i=0;i<n;++i)for(std::size_t k=0;k<n;++k)for(std::size_t j=0;j<n;++j){
      auto left=table[i][k],right=table[k][j];
      for(auto a:left)for(auto b:right)changed=table[i][j].insert(scalarProduct(a,b)).second||changed;
    }
    for(const auto &a:g.edges())if(a.label.kind==open)
      for(const auto &b:g.edges())if(b.label.kind==close&&a.label.id==b.label.id){
        auto middle=table[a.target][b.source];
        for(auto m:middle)changed=table[a.source][b.target].insert(scalarProduct(weights.at(a),scalarProduct(m,weights.at(b)))).second||changed;
      }
  }
  return table;
}
inline void randomCyclicGraphs() {
  std::mt19937 rng(4001);
  for(unsigned trial=0;trial<80;++trial){
    Graph g;for(unsigned v=0;v<4;++v)g.addVertex(v);
    for(unsigned j=0;j<8;++j) {
      const auto from=rng()%4,to=rng()%4,kind=rng()%9;
      g.addEdge(from,to,label(kind));
    }
    HistoryObserver o(2);std::map<Edge,unsigned,EdgeLess> weights;
    for(const auto &e:g.edges()){unsigned value=rng()%16;weights.emplace(e,value);o.set(e,numbered(2,value));}
    auto cs=cyclicOracle(g,weights,true),fs=cyclicOracle(g,weights,false);
    for(unsigned s=0;s<4;++s){
      auto analysis=Solver().prepare(g,o);
      auto forward=analysis.queryFrom(s);auto backward=analysis.queryTo(s);
      for(unsigned t=0;t<4;++t){
        AffineSpace c(2),f(2),rc(2),rf(2);
        for(auto v:cs[s][t])c.addPoint(numbered(2,v));
        for(auto v:fs[s][t])f.addPoint(numbered(2,v));
        for(auto v:cs[t][s])rc.addPoint(numbered(2,v));
        for(auto v:fs[t][s])rf.addPoint(numbered(2,v));
        AFF_CHECK(forward.compare(t).callHistory()==c);AFF_CHECK(forward.compare(t).fieldHistory()==f);
        AFF_CHECK(backward.compare(t).callHistory()==rc);AFF_CHECK(backward.compare(t).fieldHistory()==rf);
      }
    }
  }
}
inline void recursiveStacks() {
  AffineSemiring d(3);spds::PushdownSystem<AffineSemiring> p(d);p.addControl();
  Matrix shift(3);for(unsigned i=0;i<3;++i)shift.set(i,(i+1)%3);
  p.addRule(0,0,0,{1,0},d.lift(shift));p.addRule(0,1,0,{1,1},d.lift(shift));
  auto seed=spds::RegularSet::singleton(1,{0,{0}});auto a=spds::postStar(p,seed);
  std::vector<spds::Symbol> stack(200,1);stack.push_back(0);Matrix expected=Matrix::identity(3);
  for(unsigned i=0;i<200;++i)expected=expected*shift;
  AFF_CHECK(a.weight(0,stack)==d.lift(expected));AFF_CHECK(a.weightWithPrefix(0,{1}).rank()==2);
  spds::PushdownSystem<AffineSemiring> pops(d);pops.addControl();pops.addRule(0,1,0,{},d.lift(shift));
  auto b=spds::preStar(pops,seed);AFF_CHECK(b.weight(0,stack)==d.lift(expected));
}
inline bool sameTransitions(const spds::Automaton<AffineSemiring> &a, const spds::Automaton<AffineSemiring> &b) {
  if(a.transitions().size()!=b.transitions().size())return false;
  for(const auto &entry:a.transitions()) {
    const auto *found=b.findTransition(entry.edge);
    if(!found || found->weight!=entry.weight)return false;
  }
  return true;
}
inline void incrementalSaturation() {
  AffineSemiring d(2);spds::PushdownSystem<AffineSemiring> p(d);p.addControl();p.addControl();p.addControl();
  auto seed=spds::RegularSet::singleton(3,{0,{0}});
  auto x=Matrix::parse("11/01"),y=Matrix::parse("10/11");
  p.addRule(0,0,1,{1,0},d.lift(x));
  spds::SaturationSession<AffineSemiring> session(p,seed);
  AFF_CHECK(!session.run().accepts(2,{0}));
  session.addRule(1,1,2,{},d.lift(y));p.addRule(1,1,2,{},d.lift(y));
  throws<std::logic_error>([&]{(void)session.result();});
  AFF_CHECK(session.run().weight(2,{0})==d.lift(x*y));
  auto z=Matrix(2);session.addRule(0,0,1,{1,0},d.lift(z));p.addRule(0,0,1,{1,0},d.lift(z));
  AFF_CHECK(sameTransitions(session.run(),spds::postStar(p,seed)));
  auto target=spds::RegularSet::singleton(3,{2,{0}});spds::PushdownSystem<AffineSemiring> empty(d);
  for(unsigned i=0;i<3;++i)empty.addControl();
  spds::SaturationSession<AffineSemiring> pre(empty,target,spds::Direction::Pre);
  pre.run();for(const auto &r:p.rules())pre.addRule(r.from,r.top,r.to,r.replacement,r.weight);
  AFF_CHECK(sameTransitions(pre.run(),spds::preStar(p,target)));
}
inline void regularSeeds() {
  AffineSemiring d(2);spds::PushdownSystem<AffineSemiring> p(d);p.addControl();p.addControl();
  auto m=Matrix::parse("11/00");p.addRule(0,1,1,{},d.lift(m));
  spds::RegularSet seed(2);seed.addFinal(1);seed.addTransition(0,1,1);seed.addTransition(1,2,0);
  auto a=spds::postStar(p,seed);
  AFF_CHECK(a.weight(1,{})==d.combine(d.one(),d.lift(m)));
  AFF_CHECK(a.accepts(0,{1,2,1}));
  auto b=spds::preStar(p,seed);AFF_CHECK(b.weight(0,{1})==d.combine(d.one(),d.lift(m)));
}
inline void builderCorrelation() {
  // Figure-7 shape: one call-valid route and another field-valid route.
  SynchronizedSystem a(2);spds::SynchronizedSystem<> baseline;
  Matrix x=Matrix::parse("11/01"),i=Matrix::identity(2);
  auto call=[&](FlowNode s,FlowNode t,Statement ret,Matrix m){a.addCall(s,t,ret,m);baseline.addCall(s,t,ret);};
  auto store=[&](FlowNode s,FlowNode t,Field f){a.addStore(s,t,f,i);baseline.addStore(s,t,f);};
  auto load=[&](FlowNode s,FlowNode t,Field f){a.addLoad(s,t,f,i);baseline.addLoad(s,t,f);};
  call({0,61},{1,51},62,x);call({0,61},{2,65},63,i);
  store({1,51},{3,52},8);store({2,65},{4,66},9);
  call({3,52},{5,57},53,i);call({4,66},{5,57},67,i);
  store({5,57},{6,58},7);a.addReturn({6,58},{7,53},i);baseline.addReturn({6,58},{7,53});
  load({7,53},{8,54},7);load({8,54},{9,55},9);
  SynchronizedConfiguration seed{{0,61},{},{}};
  auto result=a.postStar(seed);AFF_CHECK(baseline.postStar(seed).mayAlias({9,55}));
  AFF_CHECK(result.compareAt({9,55}).spdsMayReach());AFF_CHECK(!result.mayAlias({9,55}));
  AFF_CHECK(result.mayAccept({{5,57},{53,62},{8}}));
  AFF_CHECK(!result.mayAccept({{5,57},{53,62},{9}}));
  AFF_CHECK(result.mayReachNode({5,57}));AFF_CHECK(!result.mayAlias({99,99}));
  auto backward=a.preStar({{5,57},{53,62},{8}});AFF_CHECK(backward.mayAccept(seed));
  throws<std::invalid_argument>([&]{a.addNormal({0,0},{0,1},Matrix(3));});
}
inline void limitsAndErrors() {
  Graph g;g.addEdge(0,1,label(0));HistoryObserver o(2);
  Options options;options.max_matrix_dimension=1;
  throws<spds::ResourceLimit>([&]{(void)Solver(options).prepare(g,o);});
  options.max_matrix_dimension=0;options.limits.max_states=1;
  throws<spds::ResourceLimit>([&]{(void)Solver(options).prepare(g,o).queryFrom(0);});
  options.limits={};options.limits.max_updates=1;
  throws<spds::ResourceLimit>([&]{(void)Solver(options).prepare(g,o).queryFrom(0);});
  auto analysis=Solver().prepare(g,o);
  throws<std::invalid_argument>([&]{(void)analysis.queryFrom(7);});
  Graph invalid;invalid.addEdge(0,1,{static_cast<LabelKind>(222),0});
  throws<std::invalid_argument>([&]{(void)Solver().prepare(invalid,o);});
  AFF_CHECK(Solver().prepare(Graph{}).analyzeAll().pairs.empty());
  AffineSemiring d(2);spds::PushdownSystem<AffineSemiring> p(d);p.addControl();p.addControl();
  auto seed=spds::RegularSet::singleton(2,{0,{0}});spds::SaturationSession<AffineSemiring> session(p,seed);
  session.run();throws<std::invalid_argument>([&]{session.addRule(0,0,1,{0},AffineSpace(3));});
  throws<std::logic_error>([&]{(void)session.run();});throws<std::logic_error>([&]{(void)session.result();});
}
inline void insertionOrder() {
  auto fixture=correlationGraph();Graph reverse;auto edges=fixture.graph.edges();std::reverse(edges.begin(),edges.end());
  for(const auto &e:edges)reverse.addEdge(e.source,e.target,e.label);
  reverse.addEdge(edges[0].source,edges[0].target,edges[0].label);
  auto a=HistoryObserver::automatic(fixture.graph),b=HistoryObserver::automatic(reverse);
  std::stringstream sa,sb;a.write(sa);b.write(sb);AFF_CHECK(sa.str()==sb.str());
  auto qa=Solver().prepare(fixture.graph,a).queryFrom(0);
  auto qb=Solver().prepare(reverse,b).queryFrom(0);
  for(auto v:reverse.vertices()) {
    AFF_CHECK(qa.compare(v).callHistory()==qb.compare(v).callHistory());
    AFF_CHECK(qa.compare(v).fieldHistory()==qb.compare(v).fieldHistory());
  }
}

inline void precisionHierarchy() {
  std::mt19937 rng(424242);
  for(unsigned trial=0;trial<80;++trial) {
    Graph g;for(unsigned v=0;v<4;++v)g.addVertex(v);
    for(unsigned j=0;j<7;++j) {
      const auto a=rng()%4,b=rng()%4,k=rng()%9;
      g.addEdge(a,b,label(k));
    }
    HistoryObserver a(2),b(2);
    for(const auto &e:g.edges()){a.set(e,randomMatrix(2,rng));b.set(e,randomMatrix(2,rng));}
    auto joint=HistoryObserver::directSum({a,b});
    auto analysis=Solver().prepare(g,joint);
    auto all=analysis.analyzeAll(ComparisonMode::Joint);
    auto independent=analysis.analyzeAll(ComparisonMode::Independent);
    auto projection=analysis.analyzeAll(ComparisonMode::Projection);
    auto baseline=spds::Solver().prepare(g).analyzeAll();
    AFF_CHECK(projection.pairs==baseline.upper_bound);
    auto small=Solver().prepare(g,a).analyzeAll();
    Options o1,o2;o1.observer={2,1};o2.observer={4,3};
    auto autoSmall=Solver(o1).prepare(g).analyzeAll();
    auto autoLarge=Solver(o2).prepare(g).analyzeAll();
    for(const auto &pair:all.pairs) {
      AFF_CHECK(independent.pairs.count(pair));AFF_CHECK(small.pairs.count(pair));
    }
    for(const auto &pair:independent.pairs)AFF_CHECK(baseline.upper_bound.count(pair));
    for(const auto &pair:autoLarge.pairs)AFF_CHECK(autoSmall.pairs.count(pair));
    for(unsigned s=0;s<4;++s){auto q=analysis.queryFrom(s);
      for(unsigned t=0;t<4;++t)AFF_CHECK(q.mayReach(t)==all.mayReach(s,t));
    }
  }
}
inline void wideCertificates() {
  std::mt19937 rng(8241);
  for(unsigned trial=0;trial<80;++trial) {
    Matrix functional(9);functional.set(8,8);functional.set(7,1);
    AffineSpace left(9),right(9);
    for(unsigned j=0;j<12;++j){
      Matrix a=randomMatrix(9,rng),b=randomMatrix(9,rng);
      a.set(8,8,a.get(7,1));b.set(8,8,!b.get(7,1));
      left.addPoint(a);right.addPoint(b);
    }
    SeparationCertificate manual{functional,false,true};AFF_CHECK(manual.verify(left,right));
    auto generated=separate(left,right);AFF_CHECK(generated && generated->verify(left,right));
    AFF_CHECK(!left.intersects(right));
  }
}
using PDSKey=std::pair<spds::State,std::vector<spds::Symbol>>;
using PDSHulls=std::map<PDSKey,AffineSpace>;
inline PDSHulls pdsOracle(const spds::PushdownSystem<AffineSemiring> &p,spds::Configuration seed) {
  PDSHulls result;
  std::function<void(spds::Configuration,Matrix)> visit;
  visit=[&](spds::Configuration c,Matrix value){
    PDSKey key{c.control,c.stack};auto found=result.find(key);
    if(found==result.end())result.emplace(key,AffineSpace::singleton(value));else found->second.addPoint(value);
    if(c.stack.empty())return;
    for(const auto &rule:p.rules())if(rule.from==c.control && rule.top==c.stack.front()){
      AFF_CHECK(rule.to>rule.from); // finite execution DAG, no artificial cutoff
      auto next=rule.replacement;next.insert(next.end(),c.stack.begin()+1,c.stack.end());
      visit({rule.to,next},value*rule.weight.representative());
    }
  };
  visit(seed,Matrix::identity(p.domain().dimension()));return result;
}
inline void generalWeightedPDS() {
  std::mt19937 rng(2907);std::size_t checked=0;
  for(unsigned trial=0;trial<80;++trial){
    AffineSemiring domain(2);spds::PushdownSystem<AffineSemiring> p(domain);
    for(unsigned i=0;i<4;++i)p.addControl();
    for(unsigned i=0;i<12;++i){
      auto from=rng()%3,to=from+1+rng()%(3-from),top=rng()%2;
      std::vector<spds::Symbol> replace;const auto n=rng()%3;
      for(unsigned j=0;j<n;++j)replace.push_back(rng()%2);
      p.addRule(from,top,to,replace,domain.lift(randomMatrix(2,rng)));
    }
    spds::Configuration seed{0,{0,1}},target{3,{1}};
    auto post=spds::postStar(p,spds::RegularSet::singleton(p.controls(),seed));
    auto pre=spds::preStar(p,spds::RegularSet::singleton(p.controls(),target));
    auto oracle=pdsOracle(p,seed);
    for(unsigned state=0;state<4;++state)for(unsigned length=0;length<=5;++length)
      for(unsigned bits=0;bits<(1U<<length);++bits){
        std::vector<spds::Symbol> word;for(unsigned j=0;j<length;++j)word.push_back((bits>>j)&1U);
        auto found=oracle.find({state,word});auto expected=found==oracle.end()?domain.zero():found->second;
        AFF_CHECK(post.weight(state,word)==expected);
        auto from=pdsOracle(p,{state,word});auto to=from.find({target.control,target.stack});
        auto backward=to==from.end()?domain.zero():to->second;
        AFF_CHECK(pre.weight(state,word)==backward);++checked;
      }
  }
  AFF_CHECK(checked==20160);std::cout<<"  exact general-PDS configurations: "<<checked<<'\n';
}

using Test = std::pair<const char*,void(*)()>;
inline const std::vector<Test> &tests() {
  static const std::vector<Test> all{
    {"matrixArithmetic",matrixArithmetic},{"canonicalAffine",canonicalAffine},
    {"exhaustiveSeparation",exhaustiveSeparation},{"semiringLaws",semiringLaws},
    {"observerConstruction",observerConstruction},{"jointCorrelation",jointCorrelation},
    {"orderSensitivity",orderSensitivity},{"crossingAndNeutral",crossingAndNeutral},
    {"exhaustiveWords",exhaustiveWords},{"randomDAGs",randomDAGs},
    {"randomCyclicGraphs",randomCyclicGraphs},{"recursiveStacks",recursiveStacks},
    {"incrementalSaturation",incrementalSaturation},{"regularSeeds",regularSeeds},
    {"builderCorrelation",builderCorrelation},{"limitsAndErrors",limitsAndErrors},
    {"insertionOrder",insertionOrder},{"precisionHierarchy",precisionHierarchy},
    {"wideCertificates",wideCertificates},{"generalWeightedPDS",generalWeightedPDS}};
  return all;
}
#undef AFF_CHECK
} // namespace lotus::cfl::interleaved_dyck::affine::test
