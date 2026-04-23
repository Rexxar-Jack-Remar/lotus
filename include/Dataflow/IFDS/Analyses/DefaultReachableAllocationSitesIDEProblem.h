#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"
#include "Dataflow/IFDS/Support/LLVMFlowHelpers.h"

#include <algorithm>
#include <vector>

#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>

namespace ifds {

// Phasar-inspired default flow functions for analyses that reason about
// reachable allocation sites / pointees represented as llvm::Value facts.
template <typename Value>
class DefaultReachableAllocationSitesIDEProblem
    : public DefaultAliasAwareIDEProblem<const llvm::Value *, Value> {
public:
  using Fact = const llvm::Value *;
  using FactSet = typename IDEProblem<Fact, Value>::FactSet;

  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const Fact &fact) override {
    (void)succ;
    if (!stmt) {
      return {};
    }

    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
      if (Store->getPointerOperand() == fact) {
        return {};
      }

      if (Store->getValueOperand() == fact ||
          may_points_to(Store->getValueOperand(), fact, Store)) {
        FactSet out;
        append_points_to_facts(Store->getPointerOperand(), out);
        out.insert(fact);
        return out;
      }
    }

    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
      if (Load->getPointerOperand() == fact ||
          may_points_to(Load->getPointerOperand(), fact, Load)) {
        return {fact, Load};
      }
    }

    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override {
    FactSet out;
    flow::map_facts_to_callee_with_policies(
        call, callee, fact, out,
        [this, call](const llvm::Value *actual, const llvm::Argument *formal,
                     const Fact &source) {
          if (actual == source) {
            return true;
          }
          return actual && source && actual->getType()->isPointerTy() &&
                 source->getType()->isPointerTy() &&
                 may_points_to(actual, source, call);
        },
        [](const llvm::Value *, const llvm::Argument *formal, const Fact &) {
          return formal;
        },
        [this](const Fact &source) { return this->is_zero_fact(source); },
        [](const Fact &source) {
          return llvm::isa_and_nonnull<llvm::GlobalValue>(source);
        },
        /*PropagateGlobals=*/true,
        /*PropagateZero=*/true);
    return out;
  }

  FactSet return_flow(const llvm::CallBase *call,
                      const llvm::Instruction *exit_inst,
                      const llvm::Instruction *return_site,
                      const llvm::Function *callee, const Fact &exit_fact,
                      const Fact &call_fact) override {
    (void)return_site;
    (void)callee;
    (void)call_fact;

    FactSet out;
    flow::map_facts_to_caller_from_exit(
        call, exit_inst, exit_fact, out,
        [this, exit_inst](const llvm::Argument *formal, const llvm::Value *,
                          const Fact &source) {
          if (!formal->getType()->isPointerTy()) {
            return false;
          }
          if (formal == source) {
            return true;
          }
          return source && !llvm::isa<llvm::Argument>(source) &&
                 may_points_to(formal, source, exit_inst);
        },
        [](const llvm::Argument *, const llvm::Value *actual, const Fact &) {
          return actual;
        },
        [this, exit_inst](const llvm::Value *ret_val, const Fact &source) {
          if (ret_val == source) {
            return true;
          }
          return ret_val && source && ret_val->getType()->isPointerTy() &&
                 source->getType()->isPointerTy() &&
                 may_points_to(ret_val, source, exit_inst);
        },
        [call](const llvm::Value *, const Fact &) { return call; },
        [this](const Fact &source) { return this->is_zero_fact(source); },
        [](const Fact &source) {
          return llvm::isa_and_nonnull<llvm::GlobalValue>(source);
        },
        /*PropagateGlobals=*/true,
        /*PropagateZero=*/true,
        [this, exit_inst](FactSet &facts) {
          populate_with_may_pointees(facts, exit_inst);
        });
    return out;
  }

  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override {
    (void)return_site;
    bool has_unknown_callee = std::any_of(callees.begin(), callees.end(),
                                          [this](const llvm::Function *callee) {
                                            return !is_function_modeled(callee);
                                          });
    if (has_unknown_callee) {
      return {fact};
    }

    FactSet out;
    flow::map_facts_alongside_callsite_with_policies(
        call, fact, out,
        [this, call](const llvm::Value *arg, const Fact &source) {
          return arg && arg->getType()->isPointerTy() &&
                 (arg == source || may_points_to(arg, source, call));
        },
        [this](const Fact &source) { return this->is_zero_fact(source); },
        [](const Fact &source) {
          return llvm::isa_and_nonnull<llvm::GlobalValue>(source);
        },
        /*PropagateGlobals=*/false,
        /*PropagateZero=*/true);
    return out;
  }

protected:
  virtual bool is_function_modeled(const llvm::Function *fun) const {
    return fun != nullptr && !fun->isDeclaration();
  }

  bool get_points_to_set(const llvm::Value *ptr,
                         std::vector<const llvm::Value *> &points_to) const {
    if (!ptr || !ptr->getType()->isPointerTy()) {
      return false;
    }
    if (!this->m_alias_analysis || !this->m_alias_analysis->isInitialized()) {
      return false;
    }
    return this->m_alias_analysis->getPointsToSet(ptr, points_to);
  }

  bool may_points_to(const llvm::Value *pointer, const llvm::Value *target,
                     const llvm::Instruction *context) const {
    (void)context;
    if (!pointer || !target) {
      return false;
    }
    if (pointer == target) {
      return true;
    }

    std::vector<const llvm::Value *> points_to;
    if (get_points_to_set(pointer, points_to)) {
      return std::any_of(points_to.begin(), points_to.end(),
                         [this, target](const llvm::Value *candidate) {
                           return candidate == target ||
                                  this->may_alias_or_equal(candidate, target);
                         });
    }

    return pointer->getType()->isPointerTy() && target->getType()->isPointerTy()
               ? this->may_alias_or_equal(pointer, target)
               : false;
  }

  void append_points_to_facts(const llvm::Value *pointer, FactSet &out) const {
    if (!pointer) {
      return;
    }

    std::vector<const llvm::Value *> points_to;
    if (get_points_to_set(pointer, points_to) && !points_to.empty()) {
      out.insert(points_to.begin(), points_to.end());
      return;
    }

    // Fall back to the storage location itself when no explicit points-to set
    // is available. This keeps the default problem usable with conservative AA
    // backends that answer pairwise alias queries but cannot enumerate
    // pointees.
    out.insert(pointer);
  }

  void populate_with_may_pointees(FactSet &facts,
                                  const llvm::Instruction *context) const {
    (void)context;
    FactSet snapshot = facts;
    for (const llvm::Value *fact : snapshot) {
      std::vector<const llvm::Value *> points_to;
      if (get_points_to_set(fact, points_to)) {
        facts.insert(points_to.begin(), points_to.end());
      }
    }
  }
};

} // namespace ifds
