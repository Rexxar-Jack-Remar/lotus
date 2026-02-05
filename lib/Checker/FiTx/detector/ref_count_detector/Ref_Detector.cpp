#include "Checker/FiTx/frontend/Framework.hpp"
#include "Checker/FiTx/frontend/State.hpp"
#include "Checker/FiTx/detector/RefDetector.hpp"

namespace {
class RefCountDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    ReferenceCounter::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<framework::FrameworkPass *> framework::FrameworkPass::passes = {
    new RefCountDetector()};
