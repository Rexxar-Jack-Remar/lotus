#include "Checker/FiTx/Detector/UseBeforeInit_Detector.h"
#include "Checker/FiTx/Frontend/State.h"

namespace UseBeforeInitialization {
void defineStates(fitx::StateManager& manager) {
  auto init_args = fitx::StateArgs("init", fitx::StateType::INIT);
  fitx::State& init = manager.createState(init_args);

  auto store_args = fitx::StateArgs("store");
  fitx::State& stored = manager.createState(store_args);

  auto ubi_args = fitx::StateArgs(
      "UBI", fitx::StateType::BUG,
      fitx::BugNotificationTiming::IMMEDIATE, false);
  fitx::State& ubi = manager.createState(ubi_args);

  auto use_rule = fitx::UseValueTransitionRule();
  manager.addTransition(init, ubi, use_rule);

  auto store_any_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::ANY);
  manager.addTransition(init, stored, store_any_rule);
}
}  // namespace UseBeforeInitialization
