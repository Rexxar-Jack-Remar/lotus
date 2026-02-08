#include "Checker/FiTx/Detector/UAF_Detector.h"

#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class UseAfterFreeDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    UseAfterFree::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<framework::FrameworkPass *> framework::FrameworkPass::passes = {
    new UseAfterFreeDetector()};
