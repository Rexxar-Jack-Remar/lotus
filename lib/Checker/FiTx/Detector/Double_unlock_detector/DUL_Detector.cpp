#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

#include "Checker/FiTx/Detector/DUL_Detector.h"

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
