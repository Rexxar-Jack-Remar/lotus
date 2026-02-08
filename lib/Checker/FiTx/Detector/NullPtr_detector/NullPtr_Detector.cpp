#include "Checker/FiTx/Detector/NullPtr_Detector.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace NullPointer {
void defineStates(framework::StateManager& manager) {
  auto init_args = framework::StateArgs("init", framework::StateType::INIT);
  framework::State& init = manager.createState(init_args);

  auto null_args = framework::StateArgs("null");
  framework::State& null = manager.createState(null_args);

  auto use_args = framework::StateArgs(
      "used", framework::StateType::BUG,
      framework::BugNotificationTiming::IMMEDIATE, false);
  framework::State& use = manager.createState(use_args);

  auto use_rule = framework::UseValueTransitionRule();
  manager.addTransition(null, use, use_rule);

  auto store_non_null_rule = framework::StoreValueTransitionRule(
      framework::StoreValueTransitionRule::NON_NULL_VAL);
  manager.addTransition(null, init, store_non_null_rule);

  auto store_null_rule = framework::StoreValueTransitionRule(
      framework::StoreValueTransitionRule::NULL_VAL);
  manager.addTransition(init, null, store_null_rule);
}
}  // namespace NullPointer

namespace {
class NullPtrDereferenceDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    NullPointer::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

// passes defined in All_Detector.cpp when building with All_Detector
