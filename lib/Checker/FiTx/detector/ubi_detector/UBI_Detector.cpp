#include "Checker/FiTx/detector/UseBeforeInit_Detector.hpp"
#include "Checker/FiTx/frontend/Framework.hpp"
#include "Checker/FiTx/frontend/State.hpp"

namespace UseBeforeInitialization {
void defineStates(framework::StateManager& manager) {
  auto init_args = framework::StateArgs("init", framework::StateType::INIT);
  framework::State& init = manager.createState(init_args);

  auto store_args = framework::StateArgs("store");
  framework::State& stored = manager.createState(store_args);

  auto ubi_args = framework::StateArgs(
      "UBI", framework::StateType::BUG,
      framework::BugNotificationTiming::IMMEDIATE, false);
  framework::State& ubi = manager.createState(ubi_args);

  auto use_rule = framework::UseValueTransitionRule();
  manager.addTransition(init, ubi, use_rule);

  auto store_any_rule = framework::StoreValueTransitionRule(
      framework::StoreValueTransitionRule::ANY);
  manager.addTransition(init, stored, store_any_rule);
}
}  // namespace UseBeforeInitialization

namespace {
class UseBeforeInitializationDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    UseBeforeInitialization::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

// passes defined in All_Detector.cpp when building with All_Detector
