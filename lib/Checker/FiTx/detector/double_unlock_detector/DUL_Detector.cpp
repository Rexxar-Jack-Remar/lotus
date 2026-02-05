#include "Checker/FiTx/frontend/Framework.hpp"
#include "Checker/FiTx/frontend/State.hpp"

#include "Checker/FiTx/detector/DUL_Detector.hpp"

namespace {
class DoubleUnlockDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    DoubleUnlock::defineStates(manager);
    addStateManager(manager);
  }
};
}  // namespace

std::vector<framework::FrameworkPass *> framework::FrameworkPass::passes = {
    new DoubleUnlockDetector()};
