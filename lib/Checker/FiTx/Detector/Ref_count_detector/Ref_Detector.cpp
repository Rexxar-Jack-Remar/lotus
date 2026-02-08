#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Detector/Ref_Detector.h"
#include "Checker/FiTx/Frontend/State.h"

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
