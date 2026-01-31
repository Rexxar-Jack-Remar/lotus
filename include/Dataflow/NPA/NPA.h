/**********************************************************************
 * Newtonian Program Analysis – generic C++14 header
 *
 * Based on OCaml NPA-PMA by Di Wang.
 *
 * Features:
 *   - Conditional expressions (T0_cond/T1_cond) with condCombine
 *   - Kleene & Newton iterators with correct differential construction
 *   - Ndet linearization: adds base values to branches
 *   - InfClos: re-marks dirty each iteration
 *   - Diff::clone: resets cached values
 *
 *********************************************************************/
#ifndef NPA_HPP
#define NPA_HPP

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Dataflow/NPA/Domains/TensorProductDomain.h"

namespace npa {

 /**********************************************************************
  * 0. helpers
  *********************************************************************/
 using Symbol = std::string;
 enum class LinearStrategy {
   Naive,         ///< Vector fixpoint (all vars updated each round)
   Worklist,     ///< Dependency-driven worklist
   SCC,          ///< SCC-based: solve in topological order, fixpoint per SCC (LCFL-aware)
   TensorProduct  ///< Lift to tensor space, solve, project back (TOPLAS 2016)
 };

 template <class T> inline void hash_combine(std::size_t& h,const T& v){
     h ^= std::hash<T>{}(v)+0x9e3779b9+(h<<6)+(h>>2);}
 
 struct Stat{ double time{}; int iters{}; };
 
/**********************************************************************
 * 1. Domain concept  (semiring)
 *********************************************************************/
template <class D> struct DomainHas {
  template <class T>
  static auto test(int)->decltype( T::zero()
                                 , T::one()
                                 , T::combine(T::zero(),T::zero())
                                 , T::extend(T::zero(),T::zero())
                                 , T::extend_lin(T::zero(),T::zero())
                                 , T::ndetCombine(T::zero(),T::zero())
                                 , T::condCombine(typename T::test_type{},T::zero(),T::zero())
                                 , T::subtract(T::zero(),T::zero())
                                 , T::equal(T::zero(),T::zero())
                                 , std::true_type{});
  template <class> static std::false_type test(...);
public:
  static constexpr bool value = std::is_same<decltype(test<D>(0)),std::true_type>::value;
};

template <class D> using DomVal = typename D::value_type;
template <class D> using DomTest = typename D::test_type;

#define NPA_REQUIRE_DOMAIN(D) static_assert(DomainHas<D>::value,"Invalid DOMAIN: missing required methods")
 
struct Dirty{ mutable bool dirty_=true; void mark(bool d=true)const{dirty_=d;}};

template <class V>
struct Optional {
  bool has{false};
  V val{};
  Optional() = default;
  Optional(const Optional&) = default;
  Optional& operator=(const Optional&) = default;
  Optional& operator=(const V& v_in) { val = v_in; has = true; return *this; }
  void reset() { has = false; }
  bool has_value() const { return has; }
  V& operator*() { return val; }
  const V& operator*() const { return val; }
};
 
template <class D> struct Exp0;
template <class D> using E0 = std::shared_ptr<Exp0<D>>;
 
template <class D>
struct Exp0 : Dirty, std::enable_shared_from_this<Exp0<D>>{
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K{Term,Seq,Call,Cond,Ndet,Hole,Concat,InfClos};
  K k;
  V c; E0<D> t; Symbol sym; T phi; E0<D> t1,t2;
  mutable Optional<V> val;
  static E0<D> term(V v){ auto e=std::make_shared<Exp0>(); e->k=Term; e->c=v; return e;}
  static E0<D> seq(V c,E0<D> t){ auto e=std::make_shared<Exp0>(); e->k=Seq; e->c=c; e->t=t; return e;}
  static E0<D> call(Symbol f,E0<D> arg){ auto e=std::make_shared<Exp0>(); e->k=Call; e->sym=f; e->t=arg; return e;}
  static E0<D> cond(T phi,E0<D> t_then,E0<D> t_else){ auto e=std::make_shared<Exp0>(); e->k=Cond; e->phi=phi; e->t1=t_then; e->t2=t_else; return e;}
  static E0<D> ndet(E0<D> a,E0<D> b){ auto e=std::make_shared<Exp0>(); e->k=Ndet; e->t1=a; e->t2=b; return e;}
  static E0<D> hole(Symbol x){ auto e=std::make_shared<Exp0>(); e->k=Hole; e->sym=x; return e;}
  static E0<D> concat(E0<D> a,Symbol x,E0<D> b){ auto e=std::make_shared<Exp0>(); e->k=Concat; e->t1=a; e->t2=b; e->sym=x; return e;}
  static E0<D> inf(E0<D> body,Symbol x){ auto e=std::make_shared<Exp0>(); e->k=InfClos; e->t=body; e->sym=x; return e;}
};
 
template <class D> struct Exp1;
template <class D> using E1 = std::shared_ptr<Exp1<D>>;
 
template <class D>
struct Exp1 : Dirty{
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K{Term,Seq,Call,Cond,Ndet,Hole,Concat,InfClos,Add,Sub};
  K k;
  V c; Symbol sym; T phi; E1<D> t,t1,t2;
  mutable Optional<V> val;
  static E1<D> term(V v){ auto e=std::make_shared<Exp1>(); e->k=Term; e->c=v; return e;}
  static E1<D> add(E1<D> a,E1<D> b){ auto e=std::make_shared<Exp1>(); e->k=Add; e->t1=a; e->t2=b; return e;}
  static E1<D> sub(E1<D> a,E1<D> b){ auto e=std::make_shared<Exp1>(); e->k=Sub; e->t1=a; e->t2=b; return e;}
  static E1<D> seq(V c,E1<D> t){ auto e=std::make_shared<Exp1>(); e->k=Seq; e->c=c; e->t=t; return e;}
  static E1<D> call(Symbol f,V c){ auto e=std::make_shared<Exp1>(); e->k=Call; e->sym=f; e->c=c; return e;}
  static E1<D> cond(T phi,E1<D> t_then,E1<D> t_else){ auto e=std::make_shared<Exp1>(); e->k=Cond; e->phi=phi; e->t1=t_then; e->t2=t_else; return e;}
  static E1<D> ndet(E1<D> a,E1<D> b){ auto e=std::make_shared<Exp1>(); e->k=Ndet; e->t1=a; e->t2=b; return e;}
  static E1<D> hole(Symbol x){ auto e=std::make_shared<Exp1>(); e->k=Hole; e->sym=x; return e;}
  static E1<D> concat(E1<D> a,Symbol x,E1<D> b){ auto e=std::make_shared<Exp1>(); e->k=Concat; e->t1=a; e->t2=b; e->sym=x; return e;}
  static E1<D> inf(E1<D> body,Symbol x){ auto e=std::make_shared<Exp1>(); e->k=InfClos; e->t=body; e->sym=x; return e;}
};

template <class D> struct DepFinder {
  using Set = std::unordered_set<Symbol>;
  static void find(const E1<D>& e, Set& deps) {
    if (!e) return;
    using K = typename Exp1<D>::K;
    switch(e->k) {
      case K::Hole: deps.insert(e->sym); break;
      case K::Call: deps.insert(e->sym); break;
      case K::Concat: deps.insert(e->sym); find(e->t1, deps); find(e->t2, deps); break;
      case K::InfClos: deps.insert(e->sym); find(e->t, deps); break;
      default:
        if(e->t) find(e->t, deps);
        if(e->t1) find(e->t1, deps);
        if(e->t2) find(e->t2, deps);
        break;
    }
  }
};

template <class D, class F>
auto fix(bool verbose, DomVal<D> init, F f){
  NPA_REQUIRE_DOMAIN(D);
  int cnt=0;
  auto last=init;
  while(true){
    auto nxt=f(last);
    if(D::equal(last,nxt)){ if(verbose) std::cerr<<"[fp] "<<cnt+1<<"\n"; return nxt;}
    last=std::move(nxt); ++cnt;
  }
}
template <class D, class Vec, class F>
Vec fix_vec(bool verbose,Vec init,F f){
  int cnt=0;
  while(true){
    Vec nxt=f(init); bool stable=true;
    for(size_t i=0;i<init.size();++i) if(!D::equal(init[i],nxt[i])){stable=false;break;}
    if(stable){ if(verbose) std::cerr<<"[fp] "<<cnt+1<<"\n"; return nxt;}
    init.swap(nxt); ++cnt;
  }
}

template <class D> struct I0{
  using V = DomVal<D>; using Map = std::unordered_map<Symbol,V>;
  static V eval(bool /*verbose*/,const Map& nu,const E0<D>& e){ mark(e); return rec(nu,{},e); }
private:
  using Env = std::unordered_map<Symbol,V>;
  static void mark(const E0<D>& e){
    if(!e) return; e->mark();
    switch(e->k){
      case Exp0<D>::Seq:   mark(e->t); break;
      case Exp0<D>::Cond:  mark(e->t1); mark(e->t2); break;
      case Exp0<D>::Ndet:  mark(e->t1); mark(e->t2); break;
      case Exp0<D>::Concat:mark(e->t1); mark(e->t2); break;
      case Exp0<D>::InfClos: mark(e->t); break;
      default: break;
    }
  }
  static V rec(const Map& nu,const Env& env,const E0<D>& e){
    if(!e->dirty_) return *e->val;
    V v{};
    switch(e->k){
      case Exp0<D>::Term: v=e->c; break;
      case Exp0<D>::Seq:  v=D::extend(e->c,rec(nu,env,e->t)); break;
      case Exp0<D>::Call: v=D::extend(nu.at(e->sym), rec(nu,env,e->t)); break;
      case Exp0<D>::Cond: v=D::condCombine(e->phi, rec(nu,env,e->t1), rec(nu,env,e->t2)); break;
      case Exp0<D>::Ndet: v=D::ndetCombine(rec(nu,env,e->t1), rec(nu,env,e->t2)); break;
      case Exp0<D>::Hole: v=env.at(e->sym); break;
      case Exp0<D>::Concat:{ auto env2=env; env2[e->sym]=rec(nu,env,e->t2); v=rec(nu,env2,e->t1); } break;
      case Exp0<D>::InfClos:{
        V init=D::zero();
        v=fix<D>(false,init,[&](V cur){ auto env2=env; env2[e->sym]=cur; mark(e->t); return rec(nu,env2,e->t); });
      } break;
    }
    e->val=v; e->mark(false); return v;
  }
};
 
template <class D> struct Diff{
  using V = DomVal<D>; using M0 = E0<D>; using M1 = E1<D>; using Map = std::unordered_map<Symbol,V>;
  static M1 build(const Map& nu,const M0& e){ return aux(nu,e,clone(e)); }
private:
  static M0 clone(const M0& e){
    auto c=std::make_shared<Exp0<D>>(*e);
    c->val.reset(); c->dirty_=true;
    if(e->t) c->t=clone(e->t);
    if(e->t1) c->t1=clone(e->t1);
    if(e->t2) c->t2=clone(e->t2);
    return c;
  }
  static M1 aux(const Map& nu,const M0& o,const M0& cur){
    using K0 = typename Exp0<D>::K;
    switch(o->k){
      case K0::Term:   return Exp1<D>::term(D::zero());
      case K0::Seq:    return Exp1<D>::seq(o->c, aux(nu,o->t,cur->t));
      case K0::Call:{
        auto dArg = aux(nu,o->t,cur->t);
        auto left = Exp1<D>::seq(nu.at(o->sym), dArg);
        assert(o->t->val.has_value());
        auto right = Exp1<D>::call(o->sym, *o->t->val);
        return Exp1<D>::add(left,right);
      }
      case K0::Cond: return Exp1<D>::cond(o->phi, aux(nu,o->t1,cur->t1), aux(nu,o->t2,cur->t2));
      case K0::Ndet:{
        auto a1=aux(nu,o->t1,cur->t1), a2=aux(nu,o->t2,cur->t2);
        assert(o->t1->val.has_value() && o->t2->val.has_value() && o->val.has_value());
        auto aug1 = Exp1<D>::add(Exp1<D>::term(*o->t1->val), a1);
        auto aug2 = Exp1<D>::add(Exp1<D>::term(*o->t2->val), a2);
        auto augmented = Exp1<D>::ndet(aug1, aug2);
        return D::idempotent ? augmented : Exp1<D>::sub(augmented, Exp1<D>::term(*o->val));
      }
      case K0::Hole:   return Exp1<D>::hole(o->sym);
      case K0::Concat: return Exp1<D>::concat(aux(nu,o->t1,cur->t1), o->sym, aux(nu,o->t2,cur->t2));
      case K0::InfClos: return Exp1<D>::inf(aux(nu,o->t,cur->t), o->sym);
    }
    return nullptr;
  }
};

template <class D>
std::vector<DomVal<D>> solve_linear_worklist_impl(bool verbose, const std::vector<std::pair<Symbol, E1<D>>>& rhs, std::vector<DomVal<D>> init);

template <class D> struct I1{
  using V = DomVal<D>; using Map = std::unordered_map<Symbol,V>;
  static V eval(bool /*verbose*/,const Map& nu,const E1<D>& e){ mark(e); return rec(nu,{},e); }
private:
  using Env=std::unordered_map<Symbol,V>;
  static void mark(const E1<D>& e){ if(!e) return; e->mark(); if(e->t) mark(e->t); if(e->t1) mark(e->t1); if(e->t2) mark(e->t2); }
  static V rec(const Map&nu,const Env&env,const E1<D>&e){
    if(!e->dirty_) return *e->val;
    V v{}; using K = typename Exp1<D>::K;
    switch(e->k){
      case K::Term: v=e->c; break;
      case K::Seq:  v=D::extend(e->c,rec(nu,env,e->t)); break;
      case K::Call: v=D::extend(nu.at(e->sym), e->c); break;
      case K::Cond: v=D::condCombine(e->phi, rec(nu,env,e->t1), rec(nu,env,e->t2)); break;
      case K::Add:  v=D::combine(rec(nu,env,e->t1),rec(nu,env,e->t2)); break;
      case K::Sub:  v=D::subtract(rec(nu,env,e->t1),rec(nu,env,e->t2)); break;
      case K::Ndet: v=D::ndetCombine(rec(nu,env,e->t1),rec(nu,env,e->t2)); break;
      case K::Hole: v=env.at(e->sym); break;
      case K::Concat:{ auto env2=env; env2[e->sym]=rec(nu,env,e->t2); v=rec(nu,env2,e->t1); } break;
      case K::InfClos:{
        V init=D::zero();
        v=fix<D>(false,init,[&](V cur){ auto env2=env; env2[e->sym]=cur; mark(e->t); return rec(nu,env2,e->t); });
      } break;
    }
    e->val=v; e->mark(false); return v;
  }
};

template <class D>
std::vector<DomVal<D>> solve_linear_worklist_impl(bool verbose, const std::vector<std::pair<Symbol, E1<D>>>& rhs, std::vector<DomVal<D>> init) {
  using V = DomVal<D>;
  std::unordered_map<Symbol, int> sym_to_idx;
  std::unordered_map<Symbol, V> env;
  for(int i=0; i<(int)rhs.size(); ++i) { sym_to_idx[rhs[i].first] = i; env[rhs[i].first] = init[i]; }
  std::vector<std::vector<int>> users(rhs.size());
  for(int i=0; i<(int)rhs.size(); ++i) {
    std::unordered_set<Symbol> deps;
    DepFinder<D>::find(rhs[i].second, deps);
    for(const auto& d : deps) if(sym_to_idx.count(d)) users[sym_to_idx[d]].push_back(i);
  }
  std::deque<int> worklist;
  std::vector<bool> in_queue(rhs.size(), false);
  for(int i=0; i<(int)rhs.size(); ++i) { worklist.push_back(i); in_queue[i] = true; }
  long steps = 0;
  while(!worklist.empty()) {
    int idx = worklist.front(); worklist.pop_front(); in_queue[idx] = false; steps++;
    V new_val = I1<D>::eval(false, env, rhs[idx].second);
    if (!D::equal(env[rhs[idx].first], new_val)) {
      env[rhs[idx].first] = new_val; init[idx] = new_val;
      for(int u : users[idx]) if(!in_queue[u]) { worklist.push_back(u); in_queue[u] = true; }
    }
  }
  if(verbose) std::cerr << "[linear-wl] steps=" << steps << "\n";
  return init;
}

template <class D>
std::vector<DomVal<D>> solve_linear_scc_impl(bool verbose, const std::vector<std::pair<Symbol, E1<D>>>& rhs, std::vector<DomVal<D>> init);

template <class D>
std::vector<DomVal<D>> solve_linear_tensor_impl(bool verbose, const std::vector<std::pair<Symbol, E1<D>>>& rhs, std::vector<DomVal<D>> init);

template <class D,class ITER> struct Solver{
  using V = DomVal<D>; using Eqn = std::pair<Symbol,E0<D>>;
  static std::pair<std::vector<std::pair<Symbol,V>>,Stat> solve(const std::vector<Eqn>& eqns,bool verbose=false,int max=-1, LinearStrategy linStrat = LinearStrategy::Worklist){
    NPA_REQUIRE_DOMAIN(D);
    std::vector<std::pair<Symbol,V>> cur;
    for(auto& e:eqns) cur.emplace_back(e.first,D::zero());
    auto tic=std::chrono::high_resolution_clock::now();
    int it=0;
    while(max<0 || it<max){
      auto nxt = ITER::run(verbose,eqns,cur,linStrat);
      bool stable=true;
      for(size_t i=0;i<cur.size();++i) if(!D::equal(cur[i].second,nxt[i].second)){stable=false;break;}
      cur.swap(nxt); ++it;
      if(stable){ if(verbose) std::cerr<<"[conv] "<<it<<"\n"; break;}
    }
    auto toc=std::chrono::high_resolution_clock::now();
    Stat st; st.iters=it; st.time=std::chrono::duration<double>(toc-tic).count();
    return {cur,st};
  }
};

template <class D> struct KleeneIter{
  using V = DomVal<D>; using Eqn = std::pair<Symbol,E0<D>>;
  static std::vector<std::pair<Symbol,V>> run(bool verbose,const std::vector<Eqn>& eqns, const std::vector<std::pair<Symbol,V>>& binds, LinearStrategy = LinearStrategy::Worklist){
    std::unordered_map<Symbol,V> nu; for(auto&b:binds) nu[b.first]=b.second;
    std::vector<std::pair<Symbol,V>> out;
    for(auto& e:eqns) out.emplace_back(e.first, I0<D>::eval(verbose,nu,e.second));
    return out;
  }
};

template <class D> struct NewtonIter{
  using V = DomVal<D>; using Eqn = std::pair<Symbol,E0<D>>;
  static std::vector<std::pair<Symbol,V>> run(bool verbose,const std::vector<Eqn>& eqns, const std::vector<std::pair<Symbol,V>>& binds, LinearStrategy linStrat = LinearStrategy::Worklist){
    std::unordered_map<Symbol,V> nu; for(auto&b:binds) nu[b.first]=b.second;
    std::vector<std::pair<Symbol,E1<D>>> rhs;
    for(auto& e:eqns){
      V v=I0<D>::eval(verbose,nu,e.second);
      auto d=Diff<D>::build(nu,e.second);
      auto base=D::idempotent? v : D::subtract(v,nu[e.first]);
      rhs.emplace_back(e.first,Exp1<D>::add(Exp1<D>::term(base),d));
    }
    std::vector<V> init(rhs.size(), D::zero()), delta;
    if (linStrat == LinearStrategy::Naive) {
      delta = fix_vec<D>(verbose,init,[&](const std::vector<V>& cur){
        std::unordered_map<Symbol,V> env;
        for(size_t i=0;i<cur.size();++i) env[rhs[i].first]=cur[i];
        std::vector<V> nxt;
        for(auto& p:rhs) nxt.push_back(I1<D>::eval(false,env,p.second));
        return nxt;
      });
    } else if (linStrat == LinearStrategy::SCC) delta = solve_linear_scc_impl<D>(verbose, rhs, init);
    else if (linStrat == LinearStrategy::TensorProduct) delta = solve_linear_tensor_impl<D>(verbose, rhs, init);
    else delta = solve_linear_worklist_impl<D>(verbose, rhs, init);
    std::vector<std::pair<Symbol,V>> out;
    for(size_t i=0;i<binds.size();++i){
      V upd = delta[i];
      V nxt = D::idempotent? upd : D::combine(binds[i].second, upd);
      out.emplace_back(binds[i].first, nxt);
    }
    return out;
  }
};

template<class D> using KleeneSolver = Solver<D,KleeneIter<D>>;
template<class D> using NewtonSolver = Solver<D,NewtonIter<D>>;

template <class D> struct Exp1ToTensor {
  using TD = TensorProductDomain<D>;
  using E1D = E1<D>;
  using E1T = E1<TD>;
  static E1T convert(const E1D& e) {
    if (!e) return nullptr;
    using K = typename Exp1<D>::K;
    using VT = typename TD::value_type;
    switch (e->k) {
      case K::Term:   return Exp1<TD>::term(VT(e->c, e->c)); break;
      case K::Seq:    return Exp1<TD>::seq(VT(e->c, e->c), convert(e->t)); break;
      case K::Call:   return Exp1<TD>::call(e->sym, VT(e->c, e->c)); break;
      case K::Cond:   return Exp1<TD>::cond(e->phi, convert(e->t1), convert(e->t2)); break;
      case K::Add:    return Exp1<TD>::add(convert(e->t1), convert(e->t2)); break;
      case K::Sub:    return Exp1<TD>::sub(convert(e->t1), convert(e->t2)); break;
      case K::Ndet:   return Exp1<TD>::ndet(convert(e->t1), convert(e->t2)); break;
      case K::Hole:   return Exp1<TD>::hole(e->sym); break;
      case K::Concat: return Exp1<TD>::concat(convert(e->t1), e->sym, convert(e->t2)); break;
      case K::InfClos: return Exp1<TD>::inf(convert(e->t), e->sym); break;
      default: return nullptr;
    }
  }
};

template <class D>
std::vector<DomVal<D>> solve_linear_tensor_impl(bool verbose, const std::vector<std::pair<Symbol, E1<D>>>& rhs, std::vector<DomVal<D>> init) {
  using TD = TensorProductDomain<D>;
  using VT = typename TD::value_type;
  std::vector<std::pair<Symbol, E1<TD>>> rhs_tensor;
  rhs_tensor.reserve(rhs.size());
  for (const auto& p : rhs) rhs_tensor.emplace_back(p.first, Exp1ToTensor<D>::convert(p.second));
  std::vector<VT> init_tensor;
  init_tensor.reserve(init.size());
  for (const auto& v : init) init_tensor.emplace_back(v, v);
  std::vector<VT> delta_tensor = solve_linear_worklist_impl<TD>(verbose, rhs_tensor, init_tensor);
  std::vector<DomVal<D>> delta;
  delta.reserve(delta_tensor.size());
  for (const auto& p : delta_tensor) delta.push_back(TD::project(p));
  return delta;
}

template <class D>
std::vector<DomVal<D>> solve_linear_scc_impl(bool verbose, const std::vector<std::pair<Symbol, E1<D>>>& rhs, std::vector<DomVal<D>> init) {
  using V = DomVal<D>;
  const int n = static_cast<int>(rhs.size());
  std::unordered_map<Symbol, int> sym_to_idx;
  for (int i = 0; i < n; ++i) sym_to_idx[rhs[i].first] = i;
  std::vector<std::vector<int>> out_edges(n);
  for (int i = 0; i < n; ++i) {
    std::unordered_set<Symbol> deps;
    DepFinder<D>::find(rhs[i].second, deps);
    for (const auto& d : deps) {
      auto it = sym_to_idx.find(d);
      if (it != sym_to_idx.end()) out_edges[i].push_back(it->second);
    }
  }
  std::vector<int> index(n, -1), low(n, -1), scc_id(n, -1);
  std::vector<int> stack;
  int idx = 0, scc_count = 0;
  std::function<void(int)> tarjan = [&](int v) {
    index[v] = low[v] = idx++;
    stack.push_back(v);
    for (int w : out_edges[v]) {
      if (index[w] == -1) { tarjan(w); low[v] = std::min(low[v], low[w]); }
      else if (scc_id[w] == -1) low[v] = std::min(low[v], index[w]);
    }
    if (low[v] == index[v]) {
      for (;;) {
        int u = stack.back(); stack.pop_back();
        scc_id[u] = scc_count;
        if (u == v) break;
      }
      ++scc_count;
    }
  };
  for (int i = 0; i < n; ++i) if (index[i] == -1) tarjan(i);
  std::vector<std::vector<int>> sccs(scc_count);
  for (int i = 0; i < n; ++i) sccs[scc_id[i]].push_back(i);
  std::vector<std::vector<int>> rev_cond(scc_count);
  for (int v = 0; v < n; ++v)
    for (int w : out_edges[v])
      if (scc_id[v] != scc_id[w]) rev_cond[scc_id[w]].push_back(scc_id[v]);
  std::vector<int> rev_in_degree(scc_count, 0);
  for (int a = 0; a < scc_count; ++a)
    for (int b : rev_cond[a]) ++rev_in_degree[b];
  std::deque<int> q;
  for (int i = 0; i < scc_count; ++i) if (rev_in_degree[i] == 0) q.push_back(i);
  std::vector<int> scc_order;
  scc_order.reserve(scc_count);
  while (!q.empty()) {
    int a = q.front(); q.pop_front();
    scc_order.push_back(a);
    for (int b : rev_cond[a]) if (--rev_in_degree[b] == 0) q.push_back(b);
  }
  std::unordered_map<Symbol, V> env;
  for (int i = 0; i < n; ++i) env[rhs[i].first] = init[i];
  long steps = 0;
  for (int sid : scc_order) {
    const auto& scc = sccs[sid];
    for (;;) {
      bool stable = true;
      for (int i : scc) {
        V new_val = I1<D>::eval(false, env, rhs[i].second);
        if (!D::equal(env[rhs[i].first], new_val)) { env[rhs[i].first] = new_val; init[i] = new_val; stable = false; }
        ++steps;
      }
      if (stable) break;
    }
  }
  if (verbose) std::cerr << "[linear-scc] steps=" << steps << " sccs=" << scc_count << "\n";
  return init;
}

} // namespace npa

#endif /* NPA_HPP */
