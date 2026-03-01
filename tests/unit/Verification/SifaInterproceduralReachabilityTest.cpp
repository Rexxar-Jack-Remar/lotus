#include "Verification/Sifa/CallGraph.h"
#include "Verification/Sifa/Domain/IntervalDomain.h"
#include "Verification/Sifa/Domain/ReachabilityDomain.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/IcfgInterpreter.h"
#include "Verification/Sifa/Sifa.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/ICallSummarizer.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>

namespace {

static llvm::BasicBlock *getBlockByName(llvm::Function &F, const char *name) {
  for (llvm::BasicBlock &BB : F) {
    if (BB.getName() == name) {
      return &BB;
    }
  }
  return nullptr;
}

class CountingPassthroughCallSummarizer final
    : public lotus::sifa::ICallSummarizer<bool> {
public:
  explicit CountingPassthroughCallSummarizer(int &calls) : calls_(calls) {}

  bool summarize(const std::string &, const bool &inputAfterCall) override {
    ++calls_;
    return inputAfterCall;
  }

private:
  int &calls_;
};

TEST(SifaInterproceduralReachability, ReachesLoiInsideDirectCallee) {
  const char *ir = R"IR(
    declare void @__VERIFIER_error()

    define void @g() {
    entry:
      call void @__VERIFIER_error()
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *entry = getBlockByName(*gFn, "entry");
  ASSERT_NE(entry, nullptr);

  EXPECT_TRUE(lotus::sifa::isReachableInterprocedural(*M, mainFn, *gFn, *entry));
}

TEST(SifaInterproceduralReachability, ReachesCalleeFromBranchingCallBlock) {
  const char *ir = R"IR(
    declare void @__VERIFIER_error()
    declare i1 @nd()

    define void @g() {
    entry:
      call void @__VERIFIER_error()
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      %c = call i1 @nd()
      br i1 %c, label %left, label %right

    left:
      ret void

    right:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *entry = getBlockByName(*gFn, "entry");
  ASSERT_NE(entry, nullptr);

  EXPECT_TRUE(lotus::sifa::isReachableInterprocedural(*M, mainFn, *gFn, *entry));
}

TEST(SifaInterproceduralReachability,
     ReachesLoiThroughIntermediateProcedureWithoutDirectLoi) {
  const char *ir = R"IR(
    declare void @__VERIFIER_error()

    define void @g() {
    entry:
      call void @__VERIFIER_error()
      ret void
    }

    define void @f() {
    entry:
      call void @g()
      ret void
    }

    define void @main() {
    entry:
      call void @f()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *entry = getBlockByName(*gFn, "entry");
  ASSERT_NE(entry, nullptr);

  EXPECT_TRUE(lotus::sifa::isReachableInterprocedural(*M, mainFn, *gFn, *entry));
}

TEST(SifaInterproceduralReachability, DetectsUnreachableNoReturnBlocksAsErrorLocations) {
  const char *ir = R"IR(
    declare void @panic() noreturn

    define void @main() {
    entry:
      call void @panic()
      unreachable
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  auto lois = lotus::sifa::CallGraph::gatherErrorLocations(*M);
  ASSERT_EQ(lois.size(), 1u);
  ASSERT_NE(lois.front().first, nullptr);
  ASSERT_NE(lois.front().second, nullptr);
  EXPECT_EQ(lois.front().first->getName(), "main");
  EXPECT_EQ(lois.front().second->getName(), "entry");
}

TEST(SifaInterproceduralReachability, SupportsMultipleInitialProcedures) {
  const char *ir = R"IR(
    declare void @__VERIFIER_error()

    define void @g() {
    entry:
      call void @__VERIFIER_error()
      ret void
    }

    define void @entry1() {
    entry:
      ret void
    }

    define void @entry2() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *entry1 = M->getFunction("entry1");
  llvm::Function *entry2 = M->getFunction("entry2");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(entry1, nullptr);
  ASSERT_NE(entry2, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *gEntry = getBlockByName(*gFn, "entry");
  ASSERT_NE(gEntry, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;
  std::vector<lotus::sifa::CallGraph::LOI> lois = {{gFn, gEntry}};
  std::vector<const llvm::Function *> entries = {entry1, entry2};
  lotus::sifa::IcfgInterpreter<bool> icfg(*M, entries, lois, stats, domain,
                                          fluid, true);
  lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, bool> storage;

  icfg.interpret(storage);

  auto it = storage.getMap().find(gEntry);
  ASSERT_NE(it, storage.getMap().end());
  EXPECT_TRUE(it->second);
}

TEST(SifaInterproceduralReachability,
     MergesDirectAndTransitiveCalleeInputsBeforeSingleLoiWrite) {
  const char *ir = R"IR(
    declare void @__VERIFIER_error()

    define void @g() {
    entry:
      call void @__VERIFIER_error()
      ret void
    }

    define void @f() {
    entry:
      call void @g()
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      call void @f()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *gEntry = getBlockByName(*gFn, "entry");
  ASSERT_NE(gEntry, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;
  std::vector<lotus::sifa::CallGraph::LOI> lois = {{gFn, gEntry}};
  std::vector<const llvm::Function *> entries = {mainFn};
  lotus::sifa::IcfgInterpreter<bool> icfg(*M, entries, lois, stats, domain,
                                          fluid, true);
  lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, bool> storage;

  EXPECT_NO_THROW(icfg.interpret(storage));

  auto it = storage.getMap().find(gEntry);
  ASSERT_NE(it, storage.getMap().end());
  EXPECT_TRUE(it->second);
}

TEST(SifaInterproceduralReachability,
     PublicGenericApiSupportsInterproceduralIntervalQueries) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *gEntry = getBlockByName(*gFn, "entry");
  ASSERT_NE(gEntry, nullptr);

  lotus::sifa::IntervalDomain domain(nullptr, nullptr);
  lotus::sifa::IntervalState result =
      lotus::sifa::analyzeInterproceduralTo<lotus::sifa::IntervalState>(
          *M, mainFn, *gFn, *gEntry, lotus::sifa::IntervalState{}, domain);

  EXPECT_FALSE(result.isBottom());
}

TEST(SifaInterproceduralReachability,
     InsertsBottomForRequestedButUnreachableLocations) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *gEntry = getBlockByName(*gFn, "entry");
  ASSERT_NE(gEntry, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;
  std::vector<lotus::sifa::CallGraph::LOI> lois = {{gFn, gEntry}};
  lotus::sifa::IcfgInterpreter<bool> icfg(*M, mainFn, lois, stats, domain,
                                          fluid, true);
  lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, bool> storage;

  icfg.interpret(storage);

  auto it = storage.getMap().find(gEntry);
  ASSERT_NE(it, storage.getMap().end());
  EXPECT_FALSE(it->second);
}

TEST(SifaInterproceduralReachability,
     AllowsCustomCallSummarizerPolicyInIcfgInterpreter) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      br label %target

    target:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);
  llvm::BasicBlock *target = getBlockByName(*mainFn, "target");
  llvm::BasicBlock *gEntry = getBlockByName(*gFn, "entry");
  ASSERT_NE(target, nullptr);
  ASSERT_NE(gEntry, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;
  std::vector<lotus::sifa::CallGraph::LOI> lois = {{mainFn, target},
                                                   {gFn, gEntry}};
  lotus::sifa::IcfgInterpreter<bool> icfg(*M, mainFn, lois, stats, domain,
                                          fluid, true);
  int callSummaryApplications = 0;
  icfg.setCallSummarizerFactory(
      [&callSummaryApplications](
          lotus::sifa::DagInterpreter<lotus::sifa::Transition, bool> &) {
        return std::unique_ptr<lotus::sifa::ICallSummarizer<bool>>(
            new CountingPassthroughCallSummarizer(callSummaryApplications));
      });
  lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, bool> storage;

  icfg.interpret(storage);

  auto it = storage.getMap().find(target);
  ASSERT_NE(it, storage.getMap().end());
  EXPECT_TRUE(it->second);
  EXPECT_GE(callSummaryApplications, 1);
}

} // namespace
