#include "Checker/Pulse/Interproc/PulseSpecialization.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <algorithm>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace pulse {

SpecializationKey SpecializationManager::computeSpecializationKey(
    const AbductiveDomain &caller_state, const llvm::CallInst *call,
    const std::vector<AbstractValue> &actual_args) {
  SpecializationKey key;

  for (size_t i = 0; i < actual_args.size() && i < call->arg_size(); ++i) {
    AbstractValue arg = actual_args[i];
    AbstractValue canon = caller_state.getCanonical(arg);
    HeapPath path;
    path.push(HeapPath::Element(HeapPath::PathElement::Pvar));

    const auto &edges = caller_state.getPostHeap().getEdges();
    auto edge_it = edges.find(canon);
    if (edge_it != edges.end()) {
      key.heap_paths_to_values[path] = canon;
    }
  }

  for (size_t i = 0; i < actual_args.size(); ++i) {
    AbstractValue arg_i = caller_state.getCanonical(actual_args[i]);
    for (size_t j = i + 1; j < actual_args.size(); ++j) {
      AbstractValue arg_j = caller_state.getCanonical(actual_args[j]);
      if (arg_i == arg_j) {
        key.aliasing_map[arg_i].insert(arg_j);
        key.aliasing_map[arg_j].insert(arg_i);
      }
    }
  }

  return key;
}

AbductiveDomain
SpecializationManager::applySpecialization(const AbductiveDomain &astate,
                                           const SpecializationKey &key) {
  AbductiveDomain specialized = astate.clone();

  for (const auto &kv : key.aliasing_map) {
    AbstractValue v1 = kv.first;
    for (AbstractValue v2 : kv.second) {
      specialized.getPathFormula().addEquality(v1, v2);
    }
  }

  for (const auto &kv : key.dynamic_types) {
    AbstractValue av = kv.first;
    (void)kv.second;
    specialized.addNeedDynamicTypeSpecialization(av);
  }

  for (const auto &kv : key.heap_paths_to_values) {
    specialized.addNeedDynamicTypeSpecialization(
        specialized.getCanonical(kv.second));
  }

  specialized.canonicalize();
  return specialized;
}

} // namespace pulse
