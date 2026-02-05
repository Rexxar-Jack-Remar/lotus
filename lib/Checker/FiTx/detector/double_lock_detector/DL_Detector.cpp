#include "Checker/FiTx/frontend/Framework.hpp"
#include "Checker/FiTx/frontend/State.hpp"

#include "Checker/FiTx/detector/DL_Detector.hpp"

namespace {
class DoubleLockDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    DoubleLock::define_states(manager);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<framework::FrameworkPass *> framework::FrameworkPass::passes = {
    new DoubleLockDetector()};
