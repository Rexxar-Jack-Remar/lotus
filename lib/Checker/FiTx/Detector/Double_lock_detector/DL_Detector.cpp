#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

#include "Checker/FiTx/Detector/DL_Detector.h"

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
