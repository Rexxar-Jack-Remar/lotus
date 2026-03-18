#include "IR/ICFG/CallGraph.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGNode.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

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

  static const CallBase *findDirectCall(const Function *F,
                                        StringRef calleeName) {
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

  static const CallBase *findSingleIndirectCall(const Function *F) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        const auto *CB = dyn_cast<CallBase>(&I);
        if (CB && !CB->getCalledFunction())
          return CB;
      }
    }
    return nullptr;
  }

  static bool callGraphHasEdge(const LTCallGraph &cg, const Function *caller,
                               const Instruction *callInst,
                               const Function *callee) {
    const LTCallGraphNode *node = cg[caller];
    for (const auto &record : *node) {
      if (record.first == callInst && record.second &&
          record.second->getFunction() == callee)
        return true;
    }
    return false;
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
    if (pair.second->getNodeKind() == SVFGK::CallMu) {
      if (pair.second->getCallSite() == call)
        ++callMuCount;
    } else if (pair.second->getNodeKind() == SVFGK::CallChi) {
      if (pair.second->getCallSite() == call)
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

TEST_F(SVFGMemorySSATest, MemoryPhiIncomingEdgesAreGuardedIndirectFlow) {
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

  bool sawIncomingIndirectPhiEdge = false;
  for (const auto &pair : *svfg) {
    auto *phi = dyn_cast<IntraMSSAPhiSVFGNode>(pair.second);
    if (!phi)
      continue;

    for (SVFGEdge *edge : phi->getInEdges()) {
      ASSERT_NE(edge, nullptr);
      EXPECT_EQ(edge->getEdgeKind(), SVFGEdgeK::IntraIndirect);
      EXPECT_TRUE(isIntraVFGEdge(edge->getEdgeKind()));
      EXPECT_TRUE(isIndirectVFGEdge(edge->getEdgeKind()));
      if (edge->getEdgeKind() == SVFGEdgeK::IntraIndirect)
        sawIncomingIndirectPhiEdge = true;
    }
  }

  EXPECT_TRUE(sawIncomingIndirectPhiEdge);
}

TEST_F(SVFGMemorySSATest, LoadMuCapturesReachingDefVersion) {
  const char *source = R"(
    @g = global i8 0

    define i32 @main() {
    entry:
      store i8 1, i8* @g
      %v = load i8, i8* @g
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const LoadMuSVFGNode *loadMu = nullptr;
  for (const auto &pair : *svfg) {
    loadMu = dyn_cast<LoadMuSVFGNode>(pair.second);
    if (loadMu)
      break;
  }

  ASSERT_NE(loadMu, nullptr);

  bool versionMatchesIncomingDef = false;
  for (SVFGEdge *edge : loadMu->getInEdges()) {
    auto *srcMem = dyn_cast<MSSASVFGNode>(edge ? edge->getSrcNode() : nullptr);
    if (!srcMem)
      continue;
    if (srcMem->getSSAVersion() == loadMu->getSSAVersion())
      versionMatchesIncomingDef = true;
  }

  EXPECT_TRUE(versionMatchesIncomingDef);
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
    EXPECT_NE(pair.second->getNodeKind(), SVFGK::CallMu);
    EXPECT_NE(pair.second->getNodeKind(), SVFGK::CallChi);
    EXPECT_NE(pair.second->getNodeKind(), SVFGK::RetMu);
    EXPECT_NE(pair.second->getNodeKind(), SVFGK::EntryChi);
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
      store i8 1, i8* %x
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

  auto *actualIn =
      dyn_cast<ActualInSVFGNode>(*svfg->getActualIns(call).begin());
  ASSERT_NE(actualIn, nullptr);

  bool actualInVersionMatchesIncomingDef = false;
  for (SVFGEdge *edge : actualIn->getInEdges()) {
    auto *srcMem = dyn_cast<MSSASVFGNode>(edge ? edge->getSrcNode() : nullptr);
    if (!srcMem)
      continue;
    if (srcMem->getSSAVersion() == actualIn->getSSAVersion())
      actualInVersionMatchesIncomingDef = true;
  }
  EXPECT_TRUE(actualInVersionMatchesIncomingDef);

  size_t callMuCount = 0;
  for (const auto &pair : *svfg) {
    if (pair.second->getNodeKind() == SVFGK::CallMu) {
      if (pair.second->getCallSite() == call)
        ++callMuCount;
    }
  }
  EXPECT_EQ(callMuCount, 0u);
}

TEST_F(SVFGMemorySSATest, InterproceduralValueNodesUseEntryExitAndReturnSite) {
  const char *source = R"(
    declare i8* @sink(...)

    define i8* @id(i8* %p, ...) {
    entry:
      ret i8* %p
    }

    define i8* @caller(i8* %q) {
    entry:
      %r = call i8* (i8*, ...) @id(i8* %q, i8* %q)
      br label %exit

    exit:
      ret i8* %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *idFn = module->getFunction("id");
  const Function *callerFn = module->getFunction("caller");
  ASSERT_NE(idFn, nullptr);
  ASSERT_NE(callerFn, nullptr);

  const CallBase *call = findDirectCall(callerFn, "id");
  ASSERT_NE(call, nullptr);

  const ICFGNode *entryNode = icfg.getFunEntryICFGNode(idFn);
  const ICFGNode *exitNode = icfg.getFunExitICFGNode(idFn);
  const ICFGNode *returnSiteNode = icfg.getRetICFGNode(call);

  bool sawFormalParm = false;
  bool sawVarArg = false;
  for (SVFGNode *node : svfg->getFormalParms(idFn)) {
    if (auto *formalParm = dyn_cast<FormalParmSVFGNode>(node)) {
      sawFormalParm = true;
      EXPECT_EQ(formalParm->getICFGNode(), entryNode);
    } else if (auto *varArg = dyn_cast<VarArgSVFGNode>(node)) {
      sawVarArg = true;
      EXPECT_EQ(varArg->getICFGNode(), entryNode);
    }
  }
  EXPECT_TRUE(sawFormalParm);
  EXPECT_TRUE(sawVarArg);

  const auto &formalRets = svfg->getFormalRets(idFn);
  ASSERT_EQ(formalRets.size(), 1u);
  auto *formalRet = dyn_cast<FormalRetSVFGNode>(*formalRets.begin());
  ASSERT_NE(formalRet, nullptr);
  EXPECT_EQ(formalRet->getICFGNode(), exitNode);

  const auto &actualRets = svfg->getActualRets(call);
  ASSERT_EQ(actualRets.size(), 1u);
  auto *actualRet = dyn_cast<ActualRetSVFGNode>(*actualRets.begin());
  ASSERT_NE(actualRet, nullptr);
  EXPECT_EQ(actualRet->getICFGNode(), returnSiteNode);
}

TEST_F(SVFGMemorySSATest, InterproceduralMemoryNodesUseExitAndReturnSite) {
  const char *source = R"(
    define void @writer(i8* %p) {
    entry:
      store i8 1, i8* %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      call void @writer(i8* %x)
      br label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  const Function *writerFn = module->getFunction("writer");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(writerFn, nullptr);
  const CallBase *call = findDirectCall(mainFn, "writer");
  ASSERT_NE(call, nullptr);

  const ICFGNode *exitNode = icfg.getFunExitICFGNode(writerFn);
  const ICFGNode *returnSiteNode = icfg.getRetICFGNode(call);

  const auto &formalOuts = svfg->getFormalOuts(writerFn);
  ASSERT_FALSE(formalOuts.empty());
  for (SVFGNode *node : formalOuts) {
    auto *formalOut = dyn_cast<FormalOutSVFGNode>(node);
    ASSERT_NE(formalOut, nullptr);
    EXPECT_EQ(formalOut->getICFGNode(), exitNode);
  }

  const auto &actualOuts = svfg->getActualOuts(call);
  ASSERT_FALSE(actualOuts.empty());
  for (SVFGNode *node : actualOuts) {
    auto *actualOut = dyn_cast<ActualOutSVFGNode>(node);
    ASSERT_NE(actualOut, nullptr);
    EXPECT_EQ(actualOut->getICFGNode(), returnSiteNode);
  }
}

TEST_F(SVFGMemorySSATest, GlobalEntryFallbackCoversAllDirectUsersWithoutMain) {
  const char *source = R"(
    @g = global i8 0

    define void @foo() {
    entry:
      store i8 1, i8* @g
      ret void
    }

    define void @bar() {
    entry:
      %v = load i8, i8* @g
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;
  cfg.includeGlobals = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *fooFn = module->getFunction("foo");
  const Function *barFn = module->getFunction("bar");
  ASSERT_NE(fooFn, nullptr);
  ASSERT_NE(barFn, nullptr);

  ASSERT_FALSE(svfg->getFormalIns(fooFn).empty());
  ASSERT_FALSE(svfg->getFormalIns(barFn).empty());
  ASSERT_EQ(svfg->getGlobalStoreNodes().size(), 1u);
  SVFGNode *storeNode = *svfg->getGlobalStoreNodes().begin();
  ASSERT_NE(storeNode, nullptr);

  auto hasIncomingFromStore = [&](const SVFGNodeSet &formalIns) {
    for (SVFGNode *node : formalIns) {
      for (SVFGEdge *edge : node->getInEdges()) {
        if (edge && edge->getSrcNode() == storeNode &&
            edge->getEdgeKind() == SVFGEdgeK::IntraIndirect) {
          return true;
        }
      }
    }
    return false;
  };

  EXPECT_TRUE(hasIncomingFromStore(svfg->getFormalIns(fooFn)));
  EXPECT_TRUE(hasIncomingFromStore(svfg->getFormalIns(barFn)));
}

TEST_F(SVFGMemorySSATest, OnTheFlyIndirectCallUpdatesRefinedCallGraph) {
  const char *source = R"(
    define void @target(i8* %p) {
    entry:
      ret void
    }

    define void @apply(void (i8*)* %fp, i8* %arg) {
    entry:
      call void %fp(i8* %arg)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = false;
  cfg.resolveIndirectCalls = false;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *applyFn = module->getFunction("apply");
  const Function *targetFn = module->getFunction("target");
  ASSERT_NE(applyFn, nullptr);
  ASSERT_NE(targetFn, nullptr);

  const CallBase *indCall = findSingleIndirectCall(applyFn);
  ASSERT_NE(indCall, nullptr);

  std::vector<SVFGEdge *> newEdges;
  EXPECT_TRUE(builder.connectCallSiteToCalleeOnTheFly(svfg.get(), indCall,
                                                      targetFn, newEdges));

  const LTCallGraph *cg = builder.getRefinedCallGraph();
  ASSERT_NE(cg, nullptr);
  EXPECT_TRUE(callGraphHasEdge(*cg, applyFn, indCall, targetFn));
}

TEST_F(SVFGMemorySSATest, MultiReturnFunctionUsesDedicatedExitNode) {
  const char *source = R"(
    define i8* @pick(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *pickFn = module->getFunction("pick");
  ASSERT_NE(pickFn, nullptr);

  const ICFGNode *exitNode = icfg.getFunExitICFGNode(pickFn);
  ASSERT_NE(exitNode, nullptr);

  const auto &formalRets = svfg->getFormalRets(pickFn);
  ASSERT_EQ(formalRets.size(), 1u);
  auto *formalRet = dyn_cast<FormalRetSVFGNode>(*formalRets.begin());
  ASSERT_NE(formalRet, nullptr);
  EXPECT_EQ(formalRet->getICFGNode(), exitNode);
}

TEST_F(SVFGMemorySSATest, NoReturnFunctionMemorySummaryUsesDedicatedExitNode) {
  const char *source = R"(
    declare void @abort() noreturn

    define void @die(i8* %p) {
    entry:
      store i8 1, i8* %p
      call void @abort()
      unreachable
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *dieFn = module->getFunction("die");
  ASSERT_NE(dieFn, nullptr);

  const ICFGNode *exitNode = icfg.getFunExitICFGNode(dieFn);
  ASSERT_NE(exitNode, nullptr);

  const auto &formalOuts = svfg->getFormalOuts(dieFn);
  ASSERT_FALSE(formalOuts.empty());
  for (SVFGNode *node : formalOuts) {
    auto *formalOut = dyn_cast<FormalOutSVFGNode>(node);
    ASSERT_NE(formalOut, nullptr);
    EXPECT_EQ(formalOut->getICFGNode(), exitNode);
  }
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
