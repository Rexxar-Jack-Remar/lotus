#include "Checker/FiTx/Detector/Leak_Detector.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class LeakDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    MemoryLeak::defineStates(manager);  
    addStateManager(manager);
  }
};

}  // namespace

std::vector<framework::FrameworkPass *> framework::FrameworkPass::passes = {
    new LeakDetector()};
