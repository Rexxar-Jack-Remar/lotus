/*
 * Author: rainoftime
 */
#include "Dataflow/WPDS/Core/GenKillTransformer.h"

namespace wpds {

GenKillTransformer::GenKillTransformer()
    : count(0), kill(DataFlowFacts::EmptySet()),
      gen(DataFlowFacts::EmptySet()) {}

GenKillTransformer::GenKillTransformer(const DataFlowFacts &kill,
                                       const DataFlowFacts &gen)
    : count(0), kill(DataFlowFacts::Diff(kill, gen)), gen(gen) {}

GenKillTransformer::GenKillTransformer(
    const DataFlowFacts &kill, const DataFlowFacts &gen,
    const std::map<Value *, DataFlowFacts> &flow)
    : count(0), kill(DataFlowFacts::Diff(kill, gen)), gen(gen), flow(flow) {}

GenKillTransformer::GenKillTransformer(
    const DataFlowFacts &k, const DataFlowFacts &g,
    const std::map<Value *, DataFlowFacts> &f, int)
    : count(1), kill(k), gen(g), flow(f) {}

GenKillTransformer *
GenKillTransformer::makeGenKillTransformer(const DataFlowFacts &kill,
                                           const DataFlowFacts &gen) {
  return makeGenKillTransformer(kill, gen, {});
}

GenKillTransformer *GenKillTransformer::makeGenKillTransformer(
    const DataFlowFacts &kill, const DataFlowFacts &gen,
    const std::map<Value *, DataFlowFacts> &flow) {

  DataFlowFacts k_normalized = DataFlowFacts::Diff(kill, gen);

  // Check if flow is empty
  bool flowEmpty = true;
  for (const auto &pair : flow) {
    if (!pair.second.isEmpty()) {
      flowEmpty = false;
      break;
    }
  }

  if (DataFlowFacts::Eq(k_normalized, DataFlowFacts::EmptySet()) &&
      DataFlowFacts::Eq(gen, DataFlowFacts::UniverseSet()) && flowEmpty) {
    return GenKillTransformer::bottom();
  } else if (DataFlowFacts::Eq(k_normalized, DataFlowFacts::EmptySet()) &&
             DataFlowFacts::Eq(gen, DataFlowFacts::EmptySet()) && flowEmpty) {
    return GenKillTransformer::one();
  } else {
    return new GenKillTransformer(k_normalized, gen, flow);
  }
}

GenKillTransformer *GenKillTransformer::one() {
  static GenKillTransformer *ONE = new GenKillTransformer(
      DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(), {}, 1);
  return ONE;
}

GenKillTransformer *GenKillTransformer::zero() {
  static GenKillTransformer *ZERO = new GenKillTransformer(
      DataFlowFacts::UniverseSet(), DataFlowFacts::EmptySet(), {}, 1);
  return ZERO;
}

GenKillTransformer *GenKillTransformer::bottom() {
  static GenKillTransformer *BOTTOM = new GenKillTransformer(
      DataFlowFacts::EmptySet(), DataFlowFacts::UniverseSet(), {}, 1);
  return BOTTOM;
}

GenKillTransformer *GenKillTransformer::extend(GenKillTransformer *y) {
  // Special cases
  if (equal(GenKillTransformer::zero()) ||
      y->equal(GenKillTransformer::zero())) {
    return GenKillTransformer::zero();
  }

  if (equal(GenKillTransformer::one())) {
    return y;
  }

  if (y->equal(GenKillTransformer::one())) {
    return this;
  }

  // General case: (f∘g)(x) = f(g(x))
  // Note: 'y' is the outer function (f), 'this' is the inner function (g)
  // extend(y) usually means this->extend(y) = y(this).
  // Let's verify standard WPDS terminology.
  // If weights are transformers w1, w2. Path w1 w2.
  // extend(w1, w2) = w1 * w2.
  // Semiring multiplication is usually composition.
  // If 'extend' is multiplication, and we have path e1 -> e2.
  // d1 = f1(d0). d2 = f2(d1). d2 = f2(f1(d0)).
  // So extend(f1, f2) should return f2 o f1.
  // Implementation of extend(y) corresponds to `y * this` or `this * y`?
  // In InterProceduralDataFlowEngine.cpp:
  // wpds.add_rule(..., transformer).
  // Default semiring uses extend(a, b).
  // Usually a is the weight of the rule, b is the weight of the rest.
  // Let's assume standard order: extend(a, b) = a * b.
  // If a is first, b is second.
  // So `this` is first, `y` is second.
  // So Result = y(this(S)).

  // K_new = K1 U K2
  DataFlowFacts temp_k = DataFlowFacts::Union(kill, y->kill);

  // G_new = (G1 \ K2) U M2(G1 \ K2) U G2
  DataFlowFacts g1_minus_k2 = DataFlowFacts::Diff(gen, y->kill);
  DataFlowFacts m2_applied; // M2(G1 \ K2)

  // Apply M2 to g1_minus_k2
  for (Value *v : g1_minus_k2.getFacts()) {
    auto it = y->flow.find(v);
    if (it != y->flow.end()) {
      m2_applied = DataFlowFacts::Union(m2_applied, it->second);
    }
  }

  DataFlowFacts temp_g = DataFlowFacts::Union(
      DataFlowFacts::Union(g1_minus_k2, m2_applied), y->gen);

  // M_new
  // M_new(x) = (M1(x) \ K2) U M2(x) U M2(M1(x) \ K2)
  std::map<Value *, DataFlowFacts> temp_flow;

  // Collect all keys from M1 and M2
  std::set<Value *> keys;
  for (auto &kv : flow)
    keys.insert(kv.first);
  for (auto &kv : y->flow)
    keys.insert(kv.first);

  for (Value *x : keys) {
    DataFlowFacts m1_x;
    if (flow.count(x))
      m1_x = flow.at(x);

    DataFlowFacts m2_x;
    if (y->flow.count(x))
      m2_x = y->flow.at(x);

    // Term 1: M1(x) \ K2
    DataFlowFacts term1 = DataFlowFacts::Diff(m1_x, y->kill);

    // Term 2: M2(x)
    DataFlowFacts term2 = m2_x;

    // Term 3: M2(M1(x) \ K2)
    DataFlowFacts term3;
    for (Value *v : term1.getFacts()) {
      if (y->flow.count(v)) {
        term3 = DataFlowFacts::Union(term3, y->flow.at(v));
      }
    }

    DataFlowFacts result_x =
        DataFlowFacts::Union(term1, DataFlowFacts::Union(term2, term3));

    if (!result_x.isEmpty()) {
      temp_flow[x] = result_x;
    }
  }

  return makeGenKillTransformer(temp_k, temp_g, temp_flow);
}

GenKillTransformer *GenKillTransformer::combine(GenKillTransformer *y) {
  // Special cases
  if (equal(GenKillTransformer::zero())) {
    return y;
  }

  if (y->equal(GenKillTransformer::zero())) {
    return this;
  }

  // General case: meet (⊕) for semiring combine — result ⊆ T1(S) ∩ T2(S).
  // So kill_meet = K1 ∪ K2 (kill more), gen_meet = G1 ∩ G2 (gen less).
  DataFlowFacts temp_k = DataFlowFacts::Union(kill, y->kill);
  DataFlowFacts temp_g = DataFlowFacts::Intersect(gen, y->gen);

  // Flow meet: for each x not in K_meet, flow_meet(x) ⊆ flow1(x) ∩ flow2(x).
  std::map<Value *, DataFlowFacts> temp_flow;
  std::set<Value *> keys;
  for (auto &kv : flow)
    keys.insert(kv.first);
  for (auto &kv : y->flow)
    keys.insert(kv.first);

  for (Value *x : keys) {
    if (temp_k.containsFact(x))
      continue;
    DataFlowFacts f1 = flow.count(x) ? flow.at(x) : DataFlowFacts::EmptySet();
    DataFlowFacts f2 =
        y->flow.count(x) ? y->flow.at(x) : DataFlowFacts::EmptySet();
    DataFlowFacts res = DataFlowFacts::Intersect(f1, f2);
    if (!res.isEmpty())
      temp_flow[x] = res;
  }

  return makeGenKillTransformer(temp_k, temp_g, temp_flow);
}

GenKillTransformer *GenKillTransformer::diff(GenKillTransformer *y) {
  // Special cases
  if (equal(GenKillTransformer::zero())) {
    return GenKillTransformer::zero();
  }

  if (y->equal(GenKillTransformer::zero())) {
    return this;
  }

  // General case: Diff not strictly defined for arbitrary maps,
  // but WPDS diff usually tries to check if y covers this.
  // For flow analysis, diff is often simple check.

  // Keep existing logic for K/G
  DataFlowFacts temp_k = DataFlowFacts::Diff(
      DataFlowFacts::UniverseSet(), DataFlowFacts::Diff(y->kill, kill));
  DataFlowFacts temp_g = DataFlowFacts::Diff(gen, y->gen);

  // Flow diff?
  // If we can't easily diff maps, we might return 'this' (safe approximation
  // for termination if used in fixpoint?) But WPDS `diff` is often used for
  // `delta = new - old`. If we assume simple subtraction for maps:
  std::map<Value *, DataFlowFacts> temp_flow;
  for (auto &kv : flow) {
    Value *k = kv.first;
    DataFlowFacts v = kv.second;
    if (y->flow.count(k)) {
      DataFlowFacts v_other = y->flow.at(k);
      DataFlowFacts d = DataFlowFacts::Diff(v, v_other);
      if (!d.isEmpty())
        temp_flow[k] = d;
    } else {
      temp_flow[k] = v;
    }
  }

  // Test if *this <= *y (i.e., diff is empty)
  // For K/G: (Universe \ (Ky \ Kx)) == Universe => Ky \ Kx is empty => Ky
  // subset Kx (Kill is reverse lattice?) Usually: this <= y means y is "bigger"
  // (more info? or higher in lattice?). In sets: y contains this. Here logic
  // seems to be: diff returns what is in 'this' but not in 'y'.

  bool k_is_empty = DataFlowFacts::Eq(
      temp_k, DataFlowFacts::UniverseSet()); // Wait, based on logic above
  bool g_is_empty = DataFlowFacts::Eq(temp_g, DataFlowFacts::EmptySet());
  bool flow_is_empty = temp_flow.empty();

  if (k_is_empty && g_is_empty && flow_is_empty) {
    return GenKillTransformer::zero();
  }

  return makeGenKillTransformer(temp_k, temp_g, temp_flow);
}

GenKillTransformer *GenKillTransformer::quasiOne() const { return one(); }

bool GenKillTransformer::equal(GenKillTransformer *y) const {
  // Handle special values
  if (this == one() && y == one())
    return true;
  if (this == zero() && y == zero())
    return true;
  if (this == bottom() && y == bottom())
    return true;

  if ((this == one() && y != one()) || (this == zero() && y != zero()) ||
      (this == bottom() && y != bottom())) {
    return false;
  }

  // Compare gen and kill sets
  if (!DataFlowFacts::Eq(kill, y->kill))
    return false;
  if (!DataFlowFacts::Eq(gen, y->gen))
    return false;

  // Compare maps
  if (flow.size() != y->flow.size())
    return false;
  for (auto &kv : flow) {
    if (!y->flow.count(kv.first))
      return false;
    if (!DataFlowFacts::Eq(kv.second, y->flow.at(kv.first)))
      return false;
  }

  return true;
}

DataFlowFacts GenKillTransformer::apply(const DataFlowFacts &input) {
  // f(S) = (S \ Kill) U (Union_{x in S \ Kill} Flow(x)) U Gen

  // 1. S \ Kill
  DataFlowFacts survivors = DataFlowFacts::Diff(input, kill);

  // 2. Flow from survivors
  DataFlowFacts flow_out;
  for (Value *v : survivors.getFacts()) {
    if (flow.count(v)) {
      flow_out = DataFlowFacts::Union(flow_out, flow.at(v));
    }
  }

  // 3. Union everything
  DataFlowFacts result = DataFlowFacts::Union(survivors, flow_out);
  return DataFlowFacts::Union(result, gen);
}

const DataFlowFacts &GenKillTransformer::getKill() const { return kill; }

const DataFlowFacts &GenKillTransformer::getGen() const { return gen; }

const std::map<Value *, DataFlowFacts> &GenKillTransformer::getFlow() const {
  return flow;
}

std::ostream &GenKillTransformer::print(std::ostream &os) const {
  os << "GenKillTransformer{kill=";
  kill.print(os);
  os << ", gen=";
  gen.print(os);
  os << ", flow={";
  bool first = true;
  for (auto &kv : flow) {
    if (!first)
      os << ", ";
    first = false;
    // Print key
    if (kv.first->hasName())
      os << kv.first->getName().str();
    else
      os << kv.first;
    os << "->";
    kv.second.print(os);
  }
  os << "}}";
  return os;
}

} // namespace wpds
