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

  EXPECT_EQ(callMuCount, 0u);
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

  for (const auto &pair : *svfg) {
    EXPECT_FALSE(isa<CallMuSVFGNode>(pair.second));
    EXPECT_FALSE(isa<CallChiSVFGNode>(pair.second));
    EXPECT_FALSE(isa<RetMuSVFGNode>(pair.second));
    EXPECT_FALSE(isa<EntryChiSVFGNode>(pair.second));
  }
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
  EXPECT_EQ(callMuCount, 0u);
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

TEST_F(SVFGMemorySSATest, VarArgKeepsDeclaredPointerParameterSeparate) {
  const char *source = R"(
    define void @sink(i32 %tag, i8* %p, ...) {
    entry:
      %v = load i8, i8* %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      call void (i32, i8*, ...) @sink(i32 0, i8* %x, i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  const Function *sinkFn = module->getFunction("sink");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(sinkFn, nullptr);
  const CallBase *call = findDirectCall(mainFn, "sink");
  ASSERT_NE(call, nullptr);

  const ActualParmSVFGNode *fixedActual = nullptr;
  const ActualParmSVFGNode *varArgActual = nullptr;
  for (SVFGNode *node : svfg->getActualParms(call)) {
    auto *actualParm = dyn_cast<ActualParmSVFGNode>(node);
    ASSERT_NE(actualParm, nullptr);
    if (actualParm->getParamIndex() == 1)
      fixedActual = actualParm;
    else if (actualParm->getParamIndex() == 2)
      varArgActual = actualParm;
  }
  ASSERT_NE(fixedActual, nullptr);
  ASSERT_NE(varArgActual, nullptr);

  const FormalParmSVFGNode *fixedFormal = nullptr;
  const VarArgSVFGNode *varArgFormal = nullptr;
  for (SVFGNode *node : svfg->getFormalParms(sinkFn)) {
    if (auto *formalParm = dyn_cast<FormalParmSVFGNode>(node)) {
      if (formalParm->getParamIndex() == 1)
        fixedFormal = formalParm;
    } else if (auto *varArg = dyn_cast<VarArgSVFGNode>(node)) {
      varArgFormal = varArg;
    }
  }
  ASSERT_NE(fixedFormal, nullptr);
  ASSERT_NE(varArgFormal, nullptr);

  bool fixedReachesFormal = false;
  bool fixedReachesVarArg = false;
  for (SVFGEdge *edge : fixedActual->getOutEdges()) {
    if (edge->getDstNode() == fixedFormal)
      fixedReachesFormal = true;
    if (edge->getDstNode() == varArgFormal)
      fixedReachesVarArg = true;
  }

  bool extraArgReachesVarArg = false;
  for (SVFGEdge *edge : varArgActual->getOutEdges()) {
    if (edge->getDstNode() == varArgFormal)
      extraArgReachesVarArg = true;
  }

  EXPECT_TRUE(fixedReachesFormal);
  EXPECT_FALSE(fixedReachesVarArg);
  EXPECT_TRUE(extraArgReachesVarArg);
}

TEST_F(SVFGMemorySSATest, FormalOutKeepsDistinctReturnPathDefs) {
  const char *source = R"(
    define void @maybe_store(i1 %cond, i8* %p) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      store i8 1, i8* %p
      ret void

    else:
      ret void
    }

    define i32 @main(i1 %cond) {
    entry:
      %x = alloca i8
      call void @maybe_store(i1 %cond, i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *callee = module->getFunction("maybe_store");
  ASSERT_NE(callee, nullptr);
  ASSERT_EQ(svfg->getFormalOuts(callee).size(), 1u);

  auto *formalOut =
      dyn_cast<FormalOutSVFGNode>(*svfg->getFormalOuts(callee).begin());
  ASSERT_NE(formalOut, nullptr);

  size_t incomingCount = 0;
  bool sawFormalIn = false;
  bool sawStoreChi = false;
  for (SVFGEdge *edge : formalOut->getInEdges()) {
    ASSERT_NE(edge, nullptr);
    ++incomingCount;
    if (isa<FormalInSVFGNode>(edge->getSrcNode()))
      sawFormalIn = true;
    if (isa<StoreChiSVFGNode>(edge->getSrcNode()))
      sawStoreChi = true;
  }

  EXPECT_EQ(incomingCount, 2u);
  EXPECT_TRUE(sawFormalIn);
  EXPECT_TRUE(sawStoreChi);
}

TEST_F(SVFGMemorySSATest, ExternalModRefCallDoesNotBacklinkActualOut) {
  const char *source = R"(
    declare void @ext(i8*)

    define i32 @main() {
    entry:
      %x = alloca i8
      store i8 1, i8* %x
      call void @ext(i8* %x)
      %v = load i8, i8* %x
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
  const CallBase *call = findDirectCall(mainFn, "ext");
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(svfg->getActualIns(call).size(), 1u);
  ASSERT_EQ(svfg->getActualOuts(call).size(), 1u);

  SVFGNode *actualIn = *svfg->getActualIns(call).begin();
  SVFGNode *actualOut = *svfg->getActualOuts(call).begin();
  ASSERT_TRUE(isa<ActualInSVFGNode>(actualIn));
  ASSERT_TRUE(isa<ActualOutSVFGNode>(actualOut));

  SVFGEdge *fallbackEdge =
      svfg->getIntraVFGEdge(actualIn, actualOut, SVFGEdgeK::IntraIndirect);
  EXPECT_EQ(fallbackEdge, nullptr);
}

TEST_F(SVFGMemorySSATest,
       GlobalEntryEdgesSeedAllCandidateEntryFunctionsWithoutMain) {
  const char *source = R"(
    @g = global i8 0

    define void @writer() {
    entry:
      store i8 1, i8* @g
      ret void
    }

    define i8 @reader() {
    entry:
      %v = load i8, i8* @g
      ret i8 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *writerFn = module->getFunction("writer");
  const Function *readerFn = module->getFunction("reader");
  ASSERT_NE(writerFn, nullptr);
  ASSERT_NE(readerFn, nullptr);
  ASSERT_FALSE(svfg->getFormalIns(writerFn).empty());
  ASSERT_FALSE(svfg->getFormalIns(readerFn).empty());

  bool writerSeeded = false;
  for (SVFGNode *node : svfg->getFormalIns(writerFn)) {
    auto *formalIn = dyn_cast<FormalInSVFGNode>(node);
    ASSERT_NE(formalIn, nullptr);
    if (!formalIn->getInEdges().empty())
      writerSeeded = true;
  }

  bool readerSeeded = false;
  for (SVFGNode *node : svfg->getFormalIns(readerFn)) {
    auto *formalIn = dyn_cast<FormalInSVFGNode>(node);
    ASSERT_NE(formalIn, nullptr);
    if (!formalIn->getInEdges().empty())
      readerSeeded = true;
  }

  EXPECT_TRUE(writerSeeded);
  EXPECT_TRUE(readerSeeded);
}

TEST_F(SVFGMemorySSATest, BinaryOperatorsReceiveDirectValueFlowEdges) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %a = add i32 1, 2
      %b = add i32 %a, 3
      ret i32 %b
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Instruction *firstAdd = nullptr;
  const Instruction *secondAdd = nullptr;
  for (const BasicBlock &bb : *mainFn) {
    for (const Instruction &inst : bb) {
      if (!isa<BinaryOperator>(&inst))
        continue;
      if (!firstAdd)
        firstAdd = &inst;
      else {
        secondAdd = &inst;
        break;
      }
    }
  }

  ASSERT_NE(firstAdd, nullptr);
  ASSERT_NE(secondAdd, nullptr);

  SVFGNode *src = svfg->getDef(firstAdd);
  SVFGNode *dst = svfg->getDef(secondAdd);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(dst, nullptr);
  ASSERT_TRUE(isa<BinaryOpSVFGNode>(src));
  ASSERT_TRUE(isa<BinaryOpSVFGNode>(dst));

  SVFGEdge *edge = svfg->getIntraVFGEdge(src, dst, SVFGEdgeK::IntraDirect);
  EXPECT_NE(edge, nullptr);
}

} // namespace
