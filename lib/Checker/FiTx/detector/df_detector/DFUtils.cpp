// Double-free typestate: init -> free -> DF (bug). Transitions: Fun Arg (kfree),
// Store ANY (e.g. malloc result stores over freed ptr). Paper Figure 3, Table 5.
#include "Checker/FiTx/detector/DF_Detector.hpp"
#include "Checker/FiTx/core/Instructions.hpp"
#include "Checker/FiTx/frontend/PropagationConstraint.hpp"
#include "Checker/FiTx/frontend/State.hpp"

namespace DoubleFree {

/// Optional: skip propagating on refcount-like calls (e.g. *put) to reduce FPs.
class OneshotCallConstraint : public framework::StatefulConstraint {
 public:
  bool shouldPropagateOnCallInst(
      std::shared_ptr<framework::CallInst> inst) override {
    std::shared_ptr<framework::Function> called_func = inst->CalledFunction();
    std::shared_ptr<framework::Function> parent =
        inst->Parent().lock()->Parent().lock();
    if (!called_func || !parent) return true;

    // If "put"  is in the function name, it is most-likely reference counted
    // function. Hence, do not consider them
    if (called_func->Name().find("put") != std::string::npos) {
      return false;
    }

    if (visited_funcs_.find(parent) == visited_funcs_.end()) {
      visited_funcs_[parent] =
          std::map<std::shared_ptr<framework::Function>,
                   std::vector<std::shared_ptr<framework::CallInst>>>();
    }

    if (visited_funcs_[parent].find(called_func) ==
        visited_funcs_[parent].end()) {
      visited_funcs_[parent][called_func] =
          std::vector<std::shared_ptr<framework::CallInst>>();
    }

    const auto& visited_insts = visited_funcs_[parent][called_func];
    if (std::find_if(visited_insts.begin(), visited_insts.end(),
                     [inst](auto stored) {
                       return *stored < *inst;
                       /* return inst != stored && inst->Arguments() ==
                        * stored->Arguments(); */
                     }) != visited_insts.end()) {
      return false;
    }

    visited_funcs_[parent][called_func].push_back(inst);
    return true;
  }

 private:
  std::map<std::shared_ptr<framework::Function>,
           std::map<std::shared_ptr<framework::Function>,
                    std::vector<std::shared_ptr<framework::CallInst>>>>
      visited_funcs_;
};

/// Define typestate FSM: init, free, DF (bug). Register transitions: call kfree
/// (init->free, free->DF), store anything (free->init). Paper Figure 3, Table 5.
void define_states(framework::StateManager& manager) {
  framework::State& init = manager.getInitState();

  // States: init (default), free (after kfree), DF = double free (bug).
  auto free_args = framework::StateArgs("free");
  framework::State& free = manager.createState(free_args);

  auto df_args = framework::StateArgs("double free", framework::StateType::BUG);
  framework::State& double_free = manager.createState(df_args);

  // Create rule for Function Arg Transition (i.e. when the value is passed
  // as the argument of specified functions)
  auto free_func_rule = framework::FunctionArgTransitionRule(free_funcs);

  manager.addTransition(init, free, free_func_rule);
  manager.addTransition(free, double_free, free_func_rule);

  // Create rule for NULL Store Transition (i.e. when the value is nulled)
  auto store_any_rule = framework::StoreValueTransitionRule(
      framework::StoreValueTransitionRule::ANY);
  store_any_rule.setConsiderNullBranch(false);

  manager.addTransition(free, init, store_any_rule);

  manager.enableStatefulConstraint(std::make_shared<OneshotCallConstraint>());
}
}  // namespace DoubleFree
