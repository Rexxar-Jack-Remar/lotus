#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

#include "Checker/FiTx/Detector/Unref_Detector.h"

namespace {
class UnrefCountDetector : public framework::FrameworkPass {
  virtual void defineStates() override {
    framework::StateManager manager;
    UnreferenceCounter::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<framework::FrameworkPass *> framework::FrameworkPass::passes = {
    new UnrefCountDetector()};
