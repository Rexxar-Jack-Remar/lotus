//===- DoubleFreeChecker.h -- Checking double-free errors---------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef DOUBLEFREECHECKER_H_
#define DOUBLEFREECHECKER_H_

#include "Checker/Saber/LeakChecker.h"

#include <string>

namespace lotus {
namespace analysis {

class DoubleFreeChecker : public LeakChecker {

public:
  DoubleFreeChecker() = default;
  virtual ~DoubleFreeChecker() = default;

  bool runOnModule(llvm::Module &M) {
    setModule(&M);
    analyze();
    return false;
  }

  void initSrcs() override;
  void initSnks() override;

  inline bool isSourceLikeFun(const std::string &funName) override {
    return SaberCheckerAPI::getCheckerAPI()->isMemDealloc(funName);
  }

  inline bool isSinkLikeFun(const std::string &funName) override {
    return SaberCheckerAPI::getCheckerAPI()->isMemDealloc(funName);
  }

  void reportBug(ProgSlice *slice) override;

  void testsValidation(ProgSlice *slice);
  void validateSuccessTests(ProgSlice *slice, const std::string &fun);
  void validateExpectedFailureTests(ProgSlice *slice, const std::string &fun);
};

} // namespace analysis
} // namespace lotus

#endif
