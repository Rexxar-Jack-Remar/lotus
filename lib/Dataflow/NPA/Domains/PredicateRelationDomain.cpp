#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"

#include "Solvers/CUDD/cudd.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <unordered_map>

namespace npa {

struct PredicateRelation::Impl {
  Impl(unsigned predicate_count_in, DdNode *root)
      : predicate_count(predicate_count_in), bdd(root) {}
  ~Impl();
  unsigned predicate_count = 0;
  DdNode *bdd = nullptr;
};

struct PredicateTensorRelation::Impl {
  Impl(unsigned predicate_count_in, DdNode *root)
      : predicate_count(predicate_count_in), bdd(root) {}
  ~Impl();
  unsigned predicate_count = 0;
  DdNode *bdd = nullptr;
};

namespace {

enum class BaseVarGroup { Next = 0, Cur = 1, Mid = 2 };
enum class TensorVarGroup {
  APrime = 0,
  A = 1,
  B = 2,
  BPrime = 3,
  TmpA = 4,
  TmpB = 5,
};

unsigned &configuredPredicateCountRef() {
  static unsigned count = 0;
  return count;
}

unsigned &configuredLocalPredicateCountRef() {
  static unsigned count = 0;
  return count;
}

unsigned activePredicateCount() {
  const unsigned count = configuredPredicateCountRef();
  assert(count > 0 &&
         "PredicateRelationDomain::configure must be called first");
  return count;
}

unsigned activeLocalPredicateCount() {
  const unsigned local_count = configuredLocalPredicateCountRef();
  assert(local_count <= activePredicateCount());
  return local_count;
}

unsigned activeGlobalPredicateCount() {
  return activePredicateCount() - activeLocalPredicateCount();
}

unsigned baseVarIndex(unsigned predicate, BaseVarGroup group) {
  return predicate * 3 + static_cast<unsigned>(group);
}

unsigned tensorVarIndex(unsigned predicate, TensorVarGroup group) {
  return predicate * 6 + static_cast<unsigned>(group);
}

unsigned baseTotalVars(unsigned predicate_count) { return predicate_count * 3; }

unsigned tensorTotalVars(unsigned predicate_count) { return predicate_count * 6; }

std::uint64_t bitMask(unsigned width) {
  return width >= 64 ? ~std::uint64_t{0}
                     : ((std::uint64_t{1} << width) - std::uint64_t{1});
}

std::uint64_t globalBits(std::uint64_t value) {
  return value & bitMask(activeGlobalPredicateCount());
}

std::uint64_t localBits(std::uint64_t value) {
  return value >> activeGlobalPredicateCount();
}

std::uint64_t composeBits(std::uint64_t globals, std::uint64_t locals) {
  return globals | (locals << activeGlobalPredicateCount());
}

struct ManagerState {
  DdManager *base_manager = nullptr;
  DdManager *tensor_manager = nullptr;
};

ManagerState &managerState(unsigned predicate_count) {
  static std::mutex mu;
  static std::unordered_map<unsigned, ManagerState> states;
  std::lock_guard<std::mutex> lock(mu);
  return states[predicate_count];
}

template <typename Fn>
DdNode *withRef(DdManager *manager, Fn &&fn) {
  DdNode *node = fn();
  Cudd_Ref(node);
  return node;
}

DdManager *getBaseManager(unsigned predicate_count) {
  ManagerState &state = managerState(predicate_count);
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  if (!state.base_manager) {
    state.base_manager = Cudd_Init(baseTotalVars(predicate_count), 0,
                                   CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
  }
  return state.base_manager;
}

DdManager *getTensorManager(unsigned predicate_count) {
  ManagerState &state = managerState(predicate_count);
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  if (!state.tensor_manager) {
    state.tensor_manager = Cudd_Init(tensorTotalVars(predicate_count), 0,
                                     CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
  }
  return state.tensor_manager;
}

DdNode *baseVarAt(unsigned predicate_count, BaseVarGroup group, unsigned idx) {
  return Cudd_bddIthVar(getBaseManager(predicate_count),
                        static_cast<int>(baseVarIndex(idx, group)));
}

DdNode *tensorVarAt(unsigned predicate_count, TensorVarGroup group, unsigned idx) {
  return Cudd_bddIthVar(getTensorManager(predicate_count),
                        static_cast<int>(tensorVarIndex(idx, group)));
}

DdNode *logicZero(DdManager *manager) { return Cudd_ReadLogicZero(manager); }

DdNode *logicOne(DdManager *manager) { return Cudd_ReadOne(manager); }

DdNode *bddAnd(DdManager *manager, DdNode *lhs, DdNode *rhs) {
  return withRef(manager, [&] { return Cudd_bddAnd(manager, lhs, rhs); });
}

DdNode *bddOr(DdManager *manager, DdNode *lhs, DdNode *rhs) {
  return withRef(manager, [&] { return Cudd_bddOr(manager, lhs, rhs); });
}

DdNode *bddXnor(DdManager *manager, DdNode *lhs, DdNode *rhs) {
  DdNode *xor_node =
      withRef(manager, [&] { return Cudd_bddXor(manager, lhs, rhs); });
  DdNode *xnor_node = withRef(manager, [&] { return Cudd_Not(xor_node); });
  Cudd_RecursiveDeref(manager, xor_node);
  return xnor_node;
}

template <class VarFn>
DdNode *cubeForVars(DdManager *manager, unsigned predicate_count, VarFn &&var_fn) {
  std::vector<DdNode *> vars;
  std::vector<int> phase;
  vars.reserve(predicate_count);
  phase.assign(predicate_count, 1);
  for (unsigned i = 0; i < predicate_count; ++i)
    vars.push_back(var_fn(i));
  return withRef(manager, [&] {
    return Cudd_bddComputeCube(manager, vars.data(), phase.data(),
                               static_cast<int>(vars.size()));
  });
}

DdNode *relationIdentityNode(unsigned predicate_count) {
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    DdNode *eq =
        bddXnor(manager,
                baseVarAt(predicate_count, BaseVarGroup::Cur, i),
                baseVarAt(predicate_count, BaseVarGroup::Next, i));
    DdNode *tmp = bddAnd(manager, node, eq);
    Cudd_RecursiveDeref(manager, eq);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return node;
}

DdNode *tensorIdentityNode(unsigned predicate_count) {
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    DdNode *eq_left = bddXnor(manager,
                              tensorVarAt(predicate_count, TensorVarGroup::APrime, i),
                              tensorVarAt(predicate_count, TensorVarGroup::A, i));
    DdNode *tmp = bddAnd(manager, node, eq_left);
    Cudd_RecursiveDeref(manager, eq_left);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;

    DdNode *eq_right = bddXnor(manager,
                               tensorVarAt(predicate_count, TensorVarGroup::B, i),
                               tensorVarAt(predicate_count, TensorVarGroup::BPrime,
                                           i));
    tmp = bddAnd(manager, node, eq_right);
    Cudd_RecursiveDeref(manager, eq_right);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return node;
}

template <typename VisitFn>
void enumerateBitVectors(unsigned width, VisitFn &&visit);
std::vector<int> assignmentVector(unsigned predicate_count);
std::vector<int> tensorAssignmentVector(unsigned predicate_count);
void setAssignment(std::vector<int> &values, unsigned predicate_count,
                   BaseVarGroup group, std::uint64_t bits);
void setTensorAssignment(std::vector<int> &values, unsigned predicate_count,
                         TensorVarGroup group, std::uint64_t bits);
template <typename GroupFn>
DdNode *transitionCubeGeneric(DdManager *manager, unsigned predicate_count,
                              GroupFn &&group_fn);

DdNode *baseComposeNode(unsigned predicate_count, DdNode *outer, DdNode *inner) {
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *result = logicZero(manager);
  Cudd_Ref(result);
  std::vector<int> values = assignmentVector(predicate_count);
  enumerateBitVectors(predicate_count, [&](std::uint64_t cur_bits) {
    enumerateBitVectors(predicate_count, [&](std::uint64_t next_bits) {
      bool reachable = false;
      enumerateBitVectors(predicate_count, [&](std::uint64_t mid_bits) {
        if (reachable)
          return;
        std::fill(values.begin(), values.end(), 0);
        setAssignment(values, predicate_count, BaseVarGroup::Cur, cur_bits);
        setAssignment(values, predicate_count, BaseVarGroup::Next, mid_bits);
        const bool outer_ok =
            Cudd_Eval(manager, outer, values.data()) != logicZero(manager);
        std::fill(values.begin(), values.end(), 0);
        setAssignment(values, predicate_count, BaseVarGroup::Cur, mid_bits);
        setAssignment(values, predicate_count, BaseVarGroup::Next, next_bits);
        const bool inner_ok =
            Cudd_Eval(manager, inner, values.data()) != logicZero(manager);
        reachable = outer_ok && inner_ok;
      });
      if (!reachable)
        return;
      DdNode *cube = transitionCubeGeneric(
          manager, predicate_count, [&](unsigned idx) {
            return std::array<std::pair<DdNode *, int>, 2>{
                std::make_pair(baseVarAt(predicate_count, BaseVarGroup::Cur, idx),
                               static_cast<int>((cur_bits >> idx) & 1U)),
                std::make_pair(
                    baseVarAt(predicate_count, BaseVarGroup::Next, idx),
                    static_cast<int>((next_bits >> idx) & 1U))};
          });
      DdNode *merged = bddOr(manager, result, cube);
      Cudd_RecursiveDeref(manager, cube);
      Cudd_RecursiveDeref(manager, result);
      result = merged;
    });
  });
  return result;
}

DdNode *tensorComposeNode(unsigned predicate_count, DdNode *outer, DdNode *inner) {
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *result = logicZero(manager);
  Cudd_Ref(result);
  std::vector<int> values = tensorAssignmentVector(predicate_count);
  enumerateBitVectors(predicate_count, [&](std::uint64_t a_prime) {
    enumerateBitVectors(predicate_count, [&](std::uint64_t b) {
      enumerateBitVectors(predicate_count, [&](std::uint64_t a) {
        enumerateBitVectors(predicate_count, [&](std::uint64_t b_prime) {
          bool reachable = false;
          enumerateBitVectors(predicate_count, [&](std::uint64_t mid_a) {
            enumerateBitVectors(predicate_count, [&](std::uint64_t mid_b) {
              if (reachable)
                return;
              std::fill(values.begin(), values.end(), 0);
              setTensorAssignment(values, predicate_count,
                                  TensorVarGroup::APrime, a_prime);
              setTensorAssignment(values, predicate_count, TensorVarGroup::B, b);
              setTensorAssignment(values, predicate_count, TensorVarGroup::A,
                                  mid_a);
              setTensorAssignment(values, predicate_count,
                                  TensorVarGroup::BPrime, mid_b);
              const bool outer_ok =
                  Cudd_Eval(manager, outer, values.data()) != logicZero(manager);

              std::fill(values.begin(), values.end(), 0);
              setTensorAssignment(values, predicate_count,
                                  TensorVarGroup::APrime, mid_a);
              setTensorAssignment(values, predicate_count, TensorVarGroup::B,
                                  mid_b);
              setTensorAssignment(values, predicate_count, TensorVarGroup::A, a);
              setTensorAssignment(values, predicate_count,
                                  TensorVarGroup::BPrime, b_prime);
              const bool inner_ok =
                  Cudd_Eval(manager, inner, values.data()) != logicZero(manager);
              reachable = outer_ok && inner_ok;
            });
          });
          if (!reachable)
            return;
          DdNode *cube = transitionCubeGeneric(
              manager, predicate_count, [&](unsigned idx) {
                return std::array<std::pair<DdNode *, int>, 4>{
                    std::make_pair(
                        tensorVarAt(predicate_count, TensorVarGroup::APrime, idx),
                        static_cast<int>((a_prime >> idx) & 1U)),
                    std::make_pair(
                        tensorVarAt(predicate_count, TensorVarGroup::B, idx),
                        static_cast<int>((b >> idx) & 1U)),
                    std::make_pair(
                        tensorVarAt(predicate_count, TensorVarGroup::A, idx),
                        static_cast<int>((a >> idx) & 1U)),
                    std::make_pair(
                        tensorVarAt(predicate_count, TensorVarGroup::BPrime, idx),
                        static_cast<int>((b_prime >> idx) & 1U))};
              });
          DdNode *merged = bddOr(manager, result, cube);
          Cudd_RecursiveDeref(manager, cube);
          Cudd_RecursiveDeref(manager, result);
          result = merged;
        });
      });
    });
  });
  return result;
}

template <typename VisitFn>
void enumerateBitVectors(unsigned width, VisitFn &&visit) {
  const std::uint64_t total =
      width >= 63 ? 0 : (std::uint64_t{1} << width);
  if (width == 0) {
    visit(std::uint64_t{0});
    return;
  }
  for (std::uint64_t value = 0; value < total; ++value)
    visit(value);
}

std::vector<int> assignmentVector(unsigned predicate_count) {
  return std::vector<int>(baseTotalVars(predicate_count), 0);
}

std::vector<int> tensorAssignmentVector(unsigned predicate_count) {
  return std::vector<int>(tensorTotalVars(predicate_count), 0);
}

void setAssignment(std::vector<int> &values, unsigned predicate_count,
                   BaseVarGroup group, std::uint64_t bits) {
  for (unsigned i = 0; i < predicate_count; ++i)
    values[baseVarIndex(i, group)] = (bits >> i) & 1U;
}

void setTensorAssignment(std::vector<int> &values, unsigned predicate_count,
                         TensorVarGroup group, std::uint64_t bits) {
  for (unsigned i = 0; i < predicate_count; ++i)
    values[tensorVarIndex(i, group)] = (bits >> i) & 1U;
}

template <typename GroupFn>
DdNode *transitionCubeGeneric(DdManager *manager, unsigned predicate_count,
                              GroupFn &&group_fn) {
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    const auto lits = group_fn(i);
    for (const auto &lit : lits) {
      DdNode *var = nullptr;
      if (lit.second == 0 || lit.second == 1) {
        var = lit.second ? lit.first : Cudd_Not(lit.first);
      } else {
        continue;
      }
      DdNode *lit_ref = withRef(manager, [&] { return var; });
      DdNode *tmp = bddAnd(manager, node, lit_ref);
      Cudd_RecursiveDeref(manager, lit_ref);
      Cudd_RecursiveDeref(manager, node);
      node = tmp;
    }
  }
  return node;
}

std::shared_ptr<PredicateRelation::Impl> makeRelationImpl(unsigned predicate_count,
                                                          DdNode *node) {
  return std::make_shared<PredicateRelation::Impl>(predicate_count, node);
}

std::shared_ptr<PredicateTensorRelation::Impl>
makeTensorImpl(unsigned predicate_count, DdNode *node) {
  return std::make_shared<PredicateTensorRelation::Impl>(predicate_count, node);
}

PredicateRelation relationFromNode(unsigned predicate_count, DdNode *node) {
  return PredicateRelation(makeRelationImpl(predicate_count, node));
}

PredicateTensorRelation tensorFromNode(unsigned predicate_count, DdNode *node) {
  return PredicateTensorRelation(makeTensorImpl(predicate_count, node));
}

const std::shared_ptr<PredicateRelation::Impl> &implOf(const PredicateRelation &value) {
  return value.impl;
}

const std::shared_ptr<PredicateTensorRelation::Impl> &
implOf(const PredicateTensorRelation &value) {
  return value.impl;
}

PredicateRelation relationFromTransitionsImpl(
    unsigned predicate_count,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> &transitions) {
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *result = logicZero(manager);
  Cudd_Ref(result);
  for (const auto &transition : transitions) {
    DdNode *cube = transitionCubeGeneric(
        manager, predicate_count, [&](unsigned idx) {
          return std::array<std::pair<DdNode *, int>, 2>{
              std::make_pair(
                  baseVarAt(predicate_count, BaseVarGroup::Cur, idx),
                  static_cast<int>((transition.first >> idx) & 1U)),
              std::make_pair(
                  baseVarAt(predicate_count, BaseVarGroup::Next, idx),
                  static_cast<int>((transition.second >> idx) & 1U))};
        });
    DdNode *merged = bddOr(manager, result, cube);
    Cudd_RecursiveDeref(manager, cube);
    Cudd_RecursiveDeref(manager, result);
    result = merged;
  }
  return relationFromNode(predicate_count, result);
}

PredicateTensorRelation tensorFromTransitionsImpl(
    unsigned predicate_count,
    const std::vector<
        std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
        &transitions) {
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *result = logicZero(manager);
  Cudd_Ref(result);
  for (const auto &transition : transitions) {
    DdNode *cube = transitionCubeGeneric(
        manager, predicate_count, [&](unsigned idx) {
          return std::array<std::pair<DdNode *, int>, 4>{
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::APrime, idx),
                  static_cast<int>((std::get<0>(transition) >> idx) & 1U)),
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::B, idx),
                  static_cast<int>((std::get<1>(transition) >> idx) & 1U)),
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::A, idx),
                  static_cast<int>((std::get<2>(transition) >> idx) & 1U)),
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::BPrime, idx),
                  static_cast<int>((std::get<3>(transition) >> idx) & 1U))};
        });
    DdNode *merged = bddOr(manager, result, cube);
    Cudd_RecursiveDeref(manager, cube);
    Cudd_RecursiveDeref(manager, result);
    result = merged;
  }
  return tensorFromNode(predicate_count, result);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>>
materializeRelationImpl(const PredicateRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  assert(predicate_count <= 63 &&
         "materialize() supports at most 63 predicates");
  DdManager *manager = getBaseManager(predicate_count);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
  std::vector<int> values = assignmentVector(predicate_count);
  enumerateBitVectors(predicate_count, [&](std::uint64_t cur_bits) {
    enumerateBitVectors(predicate_count, [&](std::uint64_t next_bits) {
      std::fill(values.begin(), values.end(), 0);
      setAssignment(values, predicate_count, BaseVarGroup::Cur, cur_bits);
      setAssignment(values, predicate_count, BaseVarGroup::Next, next_bits);
      if (Cudd_Eval(manager, implOf(relation)->bdd, values.data()) !=
          logicZero(manager)) {
        out.emplace_back(cur_bits, next_bits);
      }
    });
  });
  return out;
}

std::vector<
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
materializeTensorImpl(const PredicateTensorRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  assert(predicate_count <= 63 &&
         "tensor materialization supports at most 63 predicates");
  DdManager *manager = getTensorManager(predicate_count);
  std::vector<
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
      out;
  std::vector<int> values = tensorAssignmentVector(predicate_count);
  enumerateBitVectors(predicate_count, [&](std::uint64_t a_prime) {
    enumerateBitVectors(predicate_count, [&](std::uint64_t b) {
      enumerateBitVectors(predicate_count, [&](std::uint64_t a) {
        enumerateBitVectors(predicate_count, [&](std::uint64_t b_prime) {
          std::fill(values.begin(), values.end(), 0);
          setTensorAssignment(values, predicate_count, TensorVarGroup::APrime,
                              a_prime);
          setTensorAssignment(values, predicate_count, TensorVarGroup::B, b);
          setTensorAssignment(values, predicate_count, TensorVarGroup::A, a);
          setTensorAssignment(values, predicate_count, TensorVarGroup::BPrime,
                              b_prime);
          if (Cudd_Eval(manager, implOf(relation)->bdd, values.data()) !=
              logicZero(manager)) {
            out.emplace_back(a_prime, b, a, b_prime);
          }
        });
      });
    });
  });
  return out;
}

PredicateRelation transposeRelationImpl(const PredicateRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> transitions;
  for (const auto &transition : materializeRelationImpl(relation))
    transitions.emplace_back(transition.second, transition.first);
  return relationFromTransitionsImpl(predicate_count, transitions);
}

PredicateTensorRelation coupleRelationImpl(const PredicateRelation &lhs,
                                          const PredicateRelation &rhs) {
  const unsigned predicate_count = implOf(lhs)->predicate_count;
  assert(predicate_count == implOf(rhs)->predicate_count);
  std::vector<
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
      transitions;
  const auto lhs_transitions = materializeRelationImpl(lhs);
  const auto rhs_transitions = materializeRelationImpl(rhs);
  transitions.reserve(lhs_transitions.size() * rhs_transitions.size());
  for (const auto &lt : lhs_transitions) {
    for (const auto &rt : rhs_transitions) {
      transitions.emplace_back(lt.second, rt.first, lt.first, rt.second);
    }
  }
  return tensorFromTransitionsImpl(predicate_count, transitions);
}

PredicateRelation readoutTensorImpl(const PredicateTensorRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> transitions;
  for (const auto &tuple : materializeTensorImpl(relation)) {
    if (std::get<0>(tuple) != std::get<1>(tuple))
      continue;
    transitions.emplace_back(std::get<2>(tuple), std::get<3>(tuple));
  }
  return relationFromTransitionsImpl(predicate_count, transitions);
}

PredicateRelation projectRelationImpl(const PredicateRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  const unsigned local_count = activeLocalPredicateCount();
  if (local_count == 0)
    return relation;

  std::vector<std::pair<std::uint64_t, std::uint64_t>> projected;
  for (const auto &transition : materializeRelationImpl(relation)) {
    const std::uint64_t src_globals = globalBits(transition.first);
    const std::uint64_t dst_globals = globalBits(transition.second);
    enumerateBitVectors(local_count, [&](std::uint64_t caller_locals) {
      projected.emplace_back(composeBits(src_globals, caller_locals),
                             composeBits(dst_globals, caller_locals));
    });
  }
  return relationFromTransitionsImpl(predicate_count, projected);
}

PredicateTensorRelation projectTensorImpl(const PredicateTensorRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  const unsigned local_count = activeLocalPredicateCount();
  if (local_count == 0)
    return relation;

  std::vector<
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
      projected;
  for (const auto &tuple : materializeTensorImpl(relation)) {
    const std::uint64_t a_prime = std::get<0>(tuple);
    const std::uint64_t b = std::get<1>(tuple);
    const std::uint64_t a = std::get<2>(tuple);
    const std::uint64_t b_prime = std::get<3>(tuple);
    if (localBits(a_prime) != localBits(b))
      continue;
    const std::uint64_t a_prime_globals = globalBits(a_prime);
    const std::uint64_t b_globals = globalBits(b);
    const std::uint64_t a_globals = globalBits(a);
    const std::uint64_t b_prime_globals = globalBits(b_prime);
    enumerateBitVectors(local_count, [&](std::uint64_t caller_left) {
      enumerateBitVectors(local_count, [&](std::uint64_t caller_right) {
        projected.emplace_back(composeBits(a_prime_globals, caller_left),
                               composeBits(b_globals, caller_right),
                               composeBits(a_globals, caller_left),
                               composeBits(b_prime_globals, caller_right));
      });
    });
  }
  return tensorFromTransitionsImpl(predicate_count, projected);
}

} // namespace

PredicateRelation::Impl::~Impl() {
  if (bdd)
    Cudd_RecursiveDeref(getBaseManager(predicate_count), bdd);
}

PredicateTensorRelation::Impl::~Impl() {
  if (bdd)
    Cudd_RecursiveDeref(getTensorManager(predicate_count), bdd);
}

PredicateRelation::PredicateRelation()
    : impl(makeRelationImpl(activePredicateCount(),
                            withRef(getBaseManager(activePredicateCount()),
                                    [&] {
                                      return logicZero(
                                          getBaseManager(activePredicateCount()));
                                    }))) {}

PredicateRelation::PredicateRelation(std::shared_ptr<Impl> impl_in)
    : impl(std::move(impl_in)) {}

PredicateRelation::PredicateRelation(const PredicateRelation &) = default;
PredicateRelation::PredicateRelation(PredicateRelation &&) noexcept = default;
PredicateRelation &PredicateRelation::operator=(const PredicateRelation &) = default;
PredicateRelation &PredicateRelation::operator=(PredicateRelation &&) noexcept =
    default;
PredicateRelation::~PredicateRelation() = default;

PredicateTensorRelation::PredicateTensorRelation()
    : impl(makeTensorImpl(activePredicateCount(),
                          withRef(getTensorManager(activePredicateCount()),
                                  [&] {
                                    return logicZero(getTensorManager(
                                        activePredicateCount()));
                                  }))) {}

PredicateTensorRelation::PredicateTensorRelation(std::shared_ptr<Impl> impl_in)
    : impl(std::move(impl_in)) {}

PredicateTensorRelation::PredicateTensorRelation(const PredicateTensorRelation &) =
    default;
PredicateTensorRelation::PredicateTensorRelation(
    PredicateTensorRelation &&) noexcept = default;
PredicateTensorRelation &
PredicateTensorRelation::operator=(const PredicateTensorRelation &) = default;
PredicateTensorRelation &
PredicateTensorRelation::operator=(PredicateTensorRelation &&) noexcept = default;
PredicateTensorRelation::~PredicateTensorRelation() = default;

void PredicateRelationDomain::configure(unsigned predicate_count,
                                        unsigned local_predicate_count) {
  assert(local_predicate_count <= predicate_count);
  configuredPredicateCountRef() = predicate_count;
  configuredLocalPredicateCountRef() = local_predicate_count;
}

unsigned PredicateRelationDomain::getPredicateCount() {
  return activePredicateCount();
}

unsigned PredicateRelationDomain::getLocalPredicateCount() {
  return activeLocalPredicateCount();
}

unsigned PredicateRelationDomain::getGlobalPredicateCount() {
  return activeGlobalPredicateCount();
}

PredicateRelationDomain::value_type PredicateRelationDomain::zero() {
  const unsigned predicate_count = activePredicateCount();
  return relationFromNode(
      predicate_count,
      withRef(getBaseManager(predicate_count),
              [&] { return logicZero(getBaseManager(predicate_count)); }));
}

PredicateRelationDomain::value_type PredicateRelationDomain::one() {
  return relationFromNode(activePredicateCount(),
                          relationIdentityNode(activePredicateCount()));
}

bool PredicateRelationDomain::equal(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  return implOf(a)->bdd == implOf(b)->bdd;
}

PredicateRelationDomain::value_type
PredicateRelationDomain::combine(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  const unsigned predicate_count = implOf(a)->predicate_count;
  return relationFromNode(
      predicate_count,
      bddOr(getBaseManager(predicate_count), implOf(a)->bdd, implOf(b)->bdd));
}

PredicateRelationDomain::value_type
PredicateRelationDomain::ndetCombine(const value_type &a, const value_type &b) {
  return combine(a, b);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::condCombine(bool /*phi*/, const value_type &t,
                                     const value_type &e) {
  return combine(t, e);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::extend(const value_type &outer, const value_type &inner) {
  assert(implOf(outer)->predicate_count == implOf(inner)->predicate_count);
  const unsigned predicate_count = implOf(outer)->predicate_count;
  return relationFromNode(predicate_count,
                          baseComposeNode(predicate_count, implOf(outer)->bdd,
                                          implOf(inner)->bdd));
}

PredicateRelationDomain::value_type
PredicateRelationDomain::extend_lin(const value_type &outer,
                                    const value_type &inner) {
  return extend(outer, inner);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::subtract(const value_type &a, const value_type & /*b*/) {
  return a;
}

PredicateRelationDomain::value_type
PredicateRelationDomain::assume(unsigned predicate, bool truthy) {
  const unsigned predicate_count = activePredicateCount();
  assert(predicate < predicate_count);
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *node = relationIdentityNode(predicate_count);
  DdNode *lit =
      truthy ? baseVarAt(predicate_count, BaseVarGroup::Cur, predicate)
             : Cudd_Not(baseVarAt(predicate_count, BaseVarGroup::Cur, predicate));
  DdNode *tmp = bddAnd(manager, node, lit);
  Cudd_RecursiveDeref(manager, node);
  return relationFromNode(predicate_count, tmp);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::assignConst(unsigned predicate, bool value) {
  const unsigned predicate_count = activePredicateCount();
  assert(predicate < predicate_count);
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    DdNode *constraint = nullptr;
    if (i == predicate) {
      constraint =
          value ? baseVarAt(predicate_count, BaseVarGroup::Next, i)
                : Cudd_Not(baseVarAt(predicate_count, BaseVarGroup::Next, i));
      Cudd_Ref(constraint);
    } else {
      constraint = bddXnor(
          manager, baseVarAt(predicate_count, BaseVarGroup::Cur, i),
          baseVarAt(predicate_count, BaseVarGroup::Next, i));
    }
    DdNode *tmp = bddAnd(manager, node, constraint);
    Cudd_RecursiveDeref(manager, constraint);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return relationFromNode(predicate_count, node);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::transpose(const value_type &relation) {
  return transposeRelationImpl(relation);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::project(const value_type &relation) {
  return projectRelationImpl(relation);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::merge(const value_type &lhs, const value_type &rhs) {
  return extend(lhs, project(rhs));
}

PredicateRelationDomain::value_type
PredicateRelationDomain::fromTransitions(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> &transitions) {
  return relationFromTransitionsImpl(activePredicateCount(), transitions);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>>
PredicateRelationDomain::materialize(const value_type &relation) {
  return materializeRelationImpl(relation);
}

PredicateTensorDomain::value_type PredicateTensorDomain::zero() {
  const unsigned predicate_count = activePredicateCount();
  return tensorFromNode(
      predicate_count,
      withRef(getTensorManager(predicate_count),
              [&] { return logicZero(getTensorManager(predicate_count)); }));
}

PredicateTensorDomain::value_type PredicateTensorDomain::one() {
  return tensorFromNode(activePredicateCount(),
                        tensorIdentityNode(activePredicateCount()));
}

bool PredicateTensorDomain::equal(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  return implOf(a)->bdd == implOf(b)->bdd;
}

PredicateTensorDomain::value_type
PredicateTensorDomain::combine(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  const unsigned predicate_count = implOf(a)->predicate_count;
  return tensorFromNode(
      predicate_count,
      bddOr(getTensorManager(predicate_count), implOf(a)->bdd, implOf(b)->bdd));
}

PredicateTensorDomain::value_type
PredicateTensorDomain::ndetCombine(const value_type &a, const value_type &b) {
  return combine(a, b);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::condCombine(bool /*phi*/, const value_type &t,
                                   const value_type &e) {
  return combine(t, e);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::extend(const value_type &outer, const value_type &inner) {
  assert(implOf(outer)->predicate_count == implOf(inner)->predicate_count);
  const unsigned predicate_count = implOf(outer)->predicate_count;
  return tensorFromNode(predicate_count,
                        tensorComposeNode(predicate_count, implOf(outer)->bdd,
                                          implOf(inner)->bdd));
}

PredicateTensorDomain::value_type
PredicateTensorDomain::extend_lin(const value_type &outer,
                                  const value_type &inner) {
  return extend(outer, inner);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::subtract(const value_type &a, const value_type & /*b*/) {
  return a;
}

PredicateTensorDomain::value_type
PredicateTensorDomain::couple(const PredicateRelation &lhs,
                              const PredicateRelation &rhs) {
  return coupleRelationImpl(lhs, rhs);
}

PredicateRelation PredicateTensorDomain::readout(const value_type &relation) {
  return readoutTensorImpl(relation);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::projectT(const value_type &relation) {
  return projectTensorImpl(relation);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::merge(const value_type &lhs, const value_type &rhs) {
  return extend(lhs, projectT(rhs));
}

PredicateTensorDomain::value_type PredicateTensorDomain::fromTransitions(
    const std::vector<
        std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
        &transitions) {
  return tensorFromTransitionsImpl(activePredicateCount(), transitions);
}

std::vector<
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
PredicateTensorDomain::materialize(const value_type &relation) {
  return materializeTensorImpl(relation);
}

} // namespace npa
