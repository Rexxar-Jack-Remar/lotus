// UAF typestate definition: init, free, BUG (use-after-free). Transitions:
// init -> free on call to free_funcs; free -> BUG on use (load); free -> init on store ANY.
#include "Checker/FiTx/Detector/UAF_Detector.h"
#include "Checker/FiTx/Frontend/State.h"

namespace UseAfterFree {
/// Suppresses propagation on calls whose name contains "put" (refcount helpers).
class OneshotCallConstraint : public framework::StatefulConstraint {
 public:
virtual ~OneshotCallConstraint() = default;
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
    return true;
  }
};

void defineStates(framework::StateManager& manager) {
  framework::State& init = manager.getInitState();

  framework::StateArgs free_args("free");
  framework::State& free = manager.createState(free_args);

  // init -> free when pointer is passed to free_funcs (e.g. kfree).
  auto free_func_rule = framework::FunctionArgTransitionRule(free_funcs);
  manager.addTransition(init, free, free_func_rule);

  // free -> BUG (use-after-free) on load (UseValueTransitionRule).
  auto ubi_args =
      framework::StateArgs("Used", framework::StateType::BUG,
                           framework::BugNotificationTiming::IMMEDIATE, false);
  framework::State& uaf = manager.createState(ubi_args);

  auto use_rule = framework::UseValueTransitionRule();
  manager.addTransition(free, uaf, use_rule);

  auto store_any_rule = framework::StoreValueTransitionRule(
      framework::StoreValueTransitionRule::ANY);

  manager.addTransition(free, init, store_any_rule);
  manager.enableStatefulConstraint(std::make_shared<OneshotCallConstraint>());
}
}  // namespace UseAfterFree
