#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGOPT.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <unistd.h>

using namespace llvm;
using namespace lotus::analysis;

namespace {

class SVFGSerializerTest : public ::testing::Test {
protected:
  LLVMContext context_;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context_);
    if (!module)
      err.print("SVFGSerializerTest", errs());
    return module;
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

  static const LoadInst *findSingleLoad(const Function *F) {
    for (const BasicBlock &BB : *F)
      for (const Instruction &I : BB)
        if (const auto *LI = dyn_cast<LoadInst>(&I))
          return LI;
    return nullptr;
  }

  static const StoreInst *findSingleStore(const Function *F) {
    for (const BasicBlock &BB : *F)
      for (const Instruction &I : BB)
        if (const auto *SI = dyn_cast<StoreInst>(&I))
          return SI;
    return nullptr;
  }
};

TEST_F(SVFGSerializerTest, RoundTripsSemanticBindings) {
  const char *source = R"(
    define i8* @id(i8* %p) {
    entry:
      %v = load i8, i8* %p
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %p = alloca i8*
      store i8* %x, i8** %p
      %q = load i8*, i8** %p
      %r = call i8* @id(i8* %q)
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
  std::unique_ptr<SVFG> original(builder.build(&icfg));
  ASSERT_NE(original, nullptr);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("svfg-serializer", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(original->writeToFile(path.str().str()));

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  Function *mainFn = module->getFunction("main");
  Function *idFn = module->getFunction("id");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(idFn, nullptr);

  const LoadInst *load = findSingleLoad(mainFn);
  const StoreInst *store = findSingleStore(mainFn);
  const CallBase *call = findDirectCall(mainFn, "id");
  ASSERT_NE(load, nullptr);
  ASSERT_NE(store, nullptr);
  ASSERT_NE(call, nullptr);

  SVFGNode *loadNode = reloaded.getDef(load);
  ASSERT_NE(loadNode, nullptr);
  EXPECT_EQ(loadNode->getNodeKind(), SVFGK::Load);
  EXPECT_EQ(loadNode->getInstruction(), load);
  ASSERT_NE(loadNode->getICFGNode(), nullptr);

  const auto &loadMus = reloaded.getLoadMus(load);
  ASSERT_FALSE(loadMus.empty());
  auto *loadMu = dyn_cast<LoadMuSVFGNode>(*loadMus.begin());
  ASSERT_NE(loadMu, nullptr);
  EXPECT_EQ(loadMu->getLoadInst(), load);
  ASSERT_NE(loadMu->getICFGNode(), nullptr);

  const auto &storeChis = reloaded.getStoreChis(store);
  ASSERT_FALSE(storeChis.empty());
  auto *storeChi = dyn_cast<StoreChiSVFGNode>(*storeChis.begin());
  ASSERT_NE(storeChi, nullptr);
  EXPECT_EQ(storeChi->getStoreInst(), store);
  ASSERT_NE(storeChi->getICFGNode(), nullptr);

  const auto &actualParms = reloaded.getActualParms(call);
  ASSERT_FALSE(actualParms.empty());
  auto *actualParm = dyn_cast<ActualParmSVFGNode>(*actualParms.begin());
  ASSERT_NE(actualParm, nullptr);
  EXPECT_EQ(actualParm->getCallSite(), call);
  ASSERT_NE(actualParm->getICFGNode(), nullptr);

  const auto &actualIns = reloaded.getActualIns(call);
  ASSERT_FALSE(actualIns.empty());
  auto *actualIn = dyn_cast<ActualInSVFGNode>(*actualIns.begin());
  ASSERT_NE(actualIn, nullptr);
  EXPECT_EQ(actualIn->getCallSite(), call);

  const auto &formalParms = reloaded.getFormalParms(idFn);
  ASSERT_FALSE(formalParms.empty());
  auto *formalParm = dyn_cast<FormalParmSVFGNode>(*formalParms.begin());
  ASSERT_NE(formalParm, nullptr);
  EXPECT_EQ(formalParm->getFunction(), idFn);

  const Argument &arg0 = *idFn->arg_begin();
  EXPECT_EQ(reloaded.getValueNode(&arg0), formalParm);

  std::vector<SVFGEdge *> interEdges;
  reloaded.getInterVFEdgesForIndirectCallSite(call, idFn, interEdges);
  EXPECT_FALSE(interEdges.empty());

  const AllocaInst *allocaX = nullptr;
  for (const BasicBlock &BB : *mainFn) {
    for (const Instruction &I : BB) {
      const auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI || !AI->getAllocatedType()->isIntegerTy(8))
        continue;
      allocaX = AI;
      break;
    }
    if (allocaX)
      break;
  }
  ASSERT_NE(allocaX, nullptr);

  auto *addrNode = dyn_cast<AddrSVFGNode>(reloaded.getValueNode(allocaX));
  ASSERT_NE(addrNode, nullptr);
  ASSERT_NE(addrNode->getObjectId(), 0u);
  const auto *info = reloaded.getObjectInfo(addrNode->getObjectId());
  ASSERT_NE(info, nullptr);
  EXPECT_TRUE(info->isStack);
  EXPECT_EQ(reloaded.getObjectValue(addrNode->getObjectId()), allocaX);
}

TEST_F(SVFGSerializerTest, RoundTripsInterPhiOperands) {
  const char *source = R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %r = call i8* @id(i8* %x)
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

  SVFGOPT optimized;
  ASSERT_NE(optimized.buildAndOptimize(&icfg, cfg), nullptr);

  std::vector<uint32_t> originalInterPhiOperandCounts;
  for (const auto &pair : optimized) {
    auto *interPhi = dyn_cast<InterPhiSVFGNode>(pair.second);
    if (!interPhi)
      continue;
    originalInterPhiOperandCounts.push_back(interPhi->getOpVerNum());
  }
  std::sort(originalInterPhiOperandCounts.begin(), originalInterPhiOperandCounts.end());
  ASSERT_FALSE(originalInterPhiOperandCounts.empty());

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("svfgopt-serializer", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(optimized.writeToFile(path.str().str()));

  SVFGOPT reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  bool foundInterPhi = false;
  std::vector<uint32_t> reloadedInterPhiOperandCounts;
  for (const auto &pair : reloaded) {
    auto *interPhi = dyn_cast<InterPhiSVFGNode>(pair.second);
    if (!interPhi)
      continue;
    foundInterPhi = true;
    reloadedInterPhiOperandCounts.push_back(interPhi->getOpVerNum());
    if (interPhi->isFormalParmPHI())
      EXPECT_NE(interPhi->getFunction(), nullptr);
    if (interPhi->isActualRetPHI())
      EXPECT_NE(interPhi->getCallSite(), nullptr);
  }

  EXPECT_TRUE(foundInterPhi);
  std::sort(reloadedInterPhiOperandCounts.begin(), reloadedInterPhiOperandCounts.end());
  EXPECT_EQ(reloadedInterPhiOperandCounts, originalInterPhiOperandCounts);
}

TEST_F(SVFGSerializerTest, PreservesExplicitNullInterPhiOperand) {
  const char *source = R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  SVFG graph;
  graph.setICFG(&icfg);

  auto *phi = new InterPhiSVFGNode(graph.getNextNodeId(),
                                   icfg.getIntraBlockNode(&mainFn->getEntryBlock()),
                                   mainFn);
  phi->setOpVer(0, nullptr);
  graph.addNode(phi);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("svfg-null-interphi", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(graph.writeToFile(path.str().str()));

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  ASSERT_EQ(reloaded.getNumNodes(), 1u);
  auto *reloadedPhi = dyn_cast<InterPhiSVFGNode>(reloaded.begin()->second);
  ASSERT_NE(reloadedPhi, nullptr);
  EXPECT_TRUE(reloadedPhi->isFormalParmPHI());
  EXPECT_EQ(reloadedPhi->getFunction(), mainFn);
  EXPECT_EQ(reloadedPhi->getOpVerNum(), 1u);
  EXPECT_EQ(reloadedPhi->getOpVer(0), nullptr);
}

TEST_F(SVFGSerializerTest, RoundTripsDeferredIndirectCallState) {
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
  std::unique_ptr<SVFG> original(builder.build(&icfg));
  ASSERT_NE(original, nullptr);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-indcall-state", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(original->writeToFile(path.str().str()));

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  Function *applyFn = module->getFunction("apply");
  Function *targetFn = module->getFunction("target");
  ASSERT_NE(applyFn, nullptr);
  ASSERT_NE(targetFn, nullptr);

  const CallBase *indCall = nullptr;
  for (const BasicBlock &bb : *applyFn) {
    for (const Instruction &inst : bb) {
      const auto *cb = dyn_cast<CallBase>(&inst);
      if (cb && !cb->getCalledFunction()) {
        indCall = cb;
        break;
      }
    }
    if (indCall)
      break;
  }
  ASSERT_NE(indCall, nullptr);

  const Argument *fpArg = &*applyFn->arg_begin();
  SVFGNode *fpNode = reloaded.getValueNode(fpArg);
  ASSERT_NE(fpNode, nullptr);
  EXPECT_EQ(reloaded.getIndCallSites(fpNode->getId()).count(indCall), 1u);

  std::vector<SVFGEdge *> newEdges;
  EXPECT_TRUE(
      builder.connectCallSiteToCalleeOnTheFly(&reloaded, indCall, targetFn,
                                              newEdges));
  EXPECT_FALSE(newEdges.empty());
  EXPECT_NE(reloaded.getCallSiteId(indCall, targetFn), 0u);

  ICFGNode *callerNode = icfg.getIntraBlockNode(indCall->getParent());
  ICFGNode *calleeEntryNode = icfg.getIntraBlockNode(&targetFn->getEntryBlock());
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntryNode, nullptr);

  bool foundCallEdge = false;
  for (const auto *edge : callerNode->getOutEdges()) {
    const auto *callEdge = dyn_cast<CallCFGEdge>(edge);
    if (!callEdge)
      continue;
    if (callEdge->getDstNode() == calleeEntryNode &&
        callEdge->getCallSite() == indCall) {
      foundCallEdge = true;
      break;
    }
  }
  EXPECT_TRUE(foundCallEdge);
}

} // namespace
