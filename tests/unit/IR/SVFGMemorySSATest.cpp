#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;

namespace {

class SVFGMemorySSATest : public ::testing::Test {
protected:
  LLVMContext context_;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context_);
    if (!module)
      err.print("SVFGMemorySSATest", errs());
    return module;
  }

  std::unique_ptr<SVFG> buildSVFG(Module *module, ICFG &icfg) {
    ICFGBuilder icfgBuilder(&icfg);
    icfgBuilder.build(module);

    SVFGBuilderConfig cfg;
    cfg.usePointerAnalysis = false;
    cfg.buildMSSA = true;

    SVFGBuilder builder(cfg);
    return std::unique_ptr<SVFG>(builder.build(&icfg));
  }

  static const CallBase *findDirectCall(const Function *F, StringRef calleeName) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        const auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        const Function *callee = CB->getCalledFunction();
        if (callee && callee->getName() == calleeName)
          return CB;
      }
    }
    return nullptr;
  }
};

TEST_F(SVFGMemorySSATest, ReadOnlyCalleeDoesNotCreateCallerSideDefs) {
  const char *source = R"(
    define i8 @reader(i8* %p) {
    entry:
      %v = load i8, i8* %p
      ret i8 %v
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %v = call i8 @reader(i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const CallBase *call = findDirectCall(mainFn, "reader");
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(svfg->getActualIns(call).empty());
  EXPECT_TRUE(svfg->getActualOuts(call).empty());

  size_t callMuCount = 0;
  size_t callChiCount = 0;
  for (const auto &pair : *svfg) {
    if (const auto *mu = dyn_cast<CallMuSVFGNode>(pair.second)) {
      if (mu->getCallSite() == call)
        ++callMuCount;
    } else if (const auto *chi = dyn_cast<CallChiSVFGNode>(pair.second)) {
      if (chi->getCallSite() == call)
        ++callChiCount;
    }
  }

  EXPECT_GT(callMuCount, 0u);
  EXPECT_EQ(callChiCount, 0u);
}

TEST_F(SVFGMemorySSATest, SameReachingDefDoesNotCreateMemoryPhi) {
  const char *source = R"(
    define i32 @main(i1 %cond) {
    entry:
      %x = alloca i8
      br i1 %cond, label %join, label %mid

    mid:
      br label %join

    join:
      %v = load i8, i8* %x
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  size_t phiCount = 0;
  for (const auto &pair : *svfg) {
    if (isa<IntraMSSAPhiSVFGNode>(pair.second))
      ++phiCount;
  }

  EXPECT_EQ(phiCount, 0u);
}

TEST_F(SVFGMemorySSATest, DistinctReachingDefsCreateMemoryPhi) {
  const char *source = R"(
    define i32 @main(i1 %cond) {
    entry:
      %x = alloca i8
      br i1 %cond, label %then, label %join

    then:
      store i8 1, i8* %x
      br label %join

    join:
      %v = load i8, i8* %x
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  size_t phiCount = 0;
  bool sawTwoOperandPhi = false;
  for (const auto &pair : *svfg) {
    auto *phi = dyn_cast<IntraMSSAPhiSVFGNode>(pair.second);
    if (!phi)
      continue;
    ++phiCount;
    if (phi->getOpVerNum() == 2)
      sawTwoOperandPhi = true;
  }

  EXPECT_GT(phiCount, 0u);
  EXPECT_TRUE(sawTwoOperandPhi);
}

TEST_F(SVFGMemorySSATest, GlobalOnlyCalleeCreatesInterproceduralMemoryNodes) {
  const char *source = R"(
    @g = global i8 0

    define void @writer() {
    entry:
      store i8 1, i8* @g
      ret void
    }

    define i32 @main() {
    entry:
      call void @writer()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  const Function *writerFn = module->getFunction("writer");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(writerFn, nullptr);
  const CallBase *call = findDirectCall(mainFn, "writer");
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(svfg->getActualIns(call).empty());
  EXPECT_FALSE(svfg->getActualOuts(call).empty());
  EXPECT_FALSE(svfg->getFormalOuts(writerFn).empty());
}

TEST_F(SVFGMemorySSATest, CallsiteMemoryNodesTrackOnlyTouchedArguments) {
  const char *source = R"(
    define i8 @touch_first(i8* %a, i8* %b) {
    entry:
      %v = load i8, i8* %a
      ret i8 %v
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %y = alloca i8
      %v = call i8 @touch_first(i8* %x, i8* %y)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const CallBase *call = findDirectCall(mainFn, "touch_first");
  ASSERT_NE(call, nullptr);

  EXPECT_EQ(svfg->getActualIns(call).size(), 1u);
  EXPECT_TRUE(svfg->getActualOuts(call).empty());

  size_t callMuCount = 0;
  for (const auto &pair : *svfg) {
    if (const auto *mu = dyn_cast<CallMuSVFGNode>(pair.second)) {
      if (mu->getCallSite() == call)
        ++callMuCount;
    }
  }
  EXPECT_EQ(callMuCount, 1u);
}

TEST_F(SVFGMemorySSATest, UntouchedPointerFormalsDoNotCreateMemoryNodes) {
  const char *source = R"(
    define void @noop(i8* %p) {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      call void @noop(i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *noopFn = module->getFunction("noop");
  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(noopFn, nullptr);
  ASSERT_NE(mainFn, nullptr);
  const CallBase *call = findDirectCall(mainFn, "noop");
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(svfg->getFormalIns(noopFn).empty());
  EXPECT_TRUE(svfg->getFormalOuts(noopFn).empty());
  EXPECT_TRUE(svfg->getActualIns(call).empty());
  EXPECT_TRUE(svfg->getActualOuts(call).empty());
}

} // namespace
