#include "Checker/FiTx/detector/Leak_Detector.hpp"
#include "Checker/FiTx/frontend/Framework.hpp"
#include "Checker/FiTx/frontend/State.hpp"

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
