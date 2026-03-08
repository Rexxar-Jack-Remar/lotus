#include "Checker/Saber/SaberCondAllocator.h"
#include "Checker/Saber/SrcSnkDDA.h"
#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
using namespace lotus::analysis;

namespace {

std::unique_ptr<Module> parseModule(LLVMContext &context, const char *source) {
  SMDiagnostic err;
  auto module = parseAssemblyString(source, err, context);
  if (!module)
    err.print("SaberConditionAllocatorTest", errs());
  return module;
}

const BasicBlock *getBlock(const Module &module, StringRef functionName,
                           StringRef blockName) {
  const Function *function = module.getFunction(functionName);
  if (!function)
    return nullptr;
  for (const BasicBlock &bb : *function) {
    if (bb.getName() == blockName)
      return &bb;
  }
  return nullptr;
}

} // namespace

namespace {

class DummySrcSnkDDA final : public SrcSnkDDA {
public:
  void initSrcs() override {}
  void initSnks() override {}
  bool isSourceLikeFun(const std::string &) override { return false; }
  bool isSinkLikeFun(const std::string &) override { return false; }
  void reportBug(ProgSlice *) override {}
};

} // namespace

TEST(SaberConditionAllocatorTest, MultiSuccessorGuardsAreExhaustiveAndExclusive) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @f(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 0, label %case0
        i32 1, label %case1
      ]
    case0:
      ret i32 0
    case1:
      ret i32 1
    default:
      ret i32 2
    }
  )");
  ASSERT_NE(module, nullptr);

  const BasicBlock *entry = getBlock(*module, "f", "entry");
  ASSERT_NE(entry, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(module.get());
  allocator.allocate();

  std::vector<SaberCondAllocator::Condition> guards;
  auto disjunction = allocator.getFalseCond();
  for (const BasicBlock *succ : successors(entry)) {
    auto guard = allocator.getBranchCond(entry, succ);
    guards.push_back(guard);
    disjunction = allocator.condOr(disjunction, guard);
  }

  ASSERT_EQ(guards.size(), 3u);
  EXPECT_TRUE(
      allocator.isEquivalentBranchCond(disjunction, allocator.getTrueCond()));
  EXPECT_TRUE(allocator.isEquivalentBranchCond(
      allocator.condAnd(guards[0], guards[1]), allocator.getFalseCond()));
  EXPECT_TRUE(allocator.isEquivalentBranchCond(
      allocator.condAnd(guards[0], guards[2]), allocator.getFalseCond()));
  EXPECT_TRUE(allocator.isEquivalentBranchCond(
      allocator.condAnd(guards[1], guards[2]), allocator.getFalseCond()));
}

TEST(SaberConditionAllocatorTest, ResetDropsConditionStateBetweenModules) {
  LLVMContext context;
  auto firstModule = parseModule(context, R"(
    define void @first(i1 %cond) {
    entry:
      br i1 %cond, label %lhs, label %rhs
    lhs:
      ret void
    rhs:
      ret void
    }
  )");
  auto secondModule = parseModule(context, R"(
    define void @second(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 0, label %case0
        i32 1, label %case1
      ]
    case0:
      ret void
    case1:
      ret void
    default:
      ret void
    }
  )");
  ASSERT_NE(firstModule, nullptr);
  ASSERT_NE(secondModule, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(firstModule.get());
  allocator.allocate();
  EXPECT_EQ(allocator.getCondNum(), 1u);

  allocator.reset();
  allocator.setModule(secondModule.get());
  allocator.allocate();
  EXPECT_EQ(allocator.getCondNum(), 2u);
}

TEST(SaberConditionAllocatorTest, LoopWithOnlyProgramExitEdgesHasNoNormalExitGuard) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @abort()

    define void @f(i1 %cond) {
    entry:
      br label %loop
    loop:
      br i1 %cond, label %abortbb, label %loop
    abortbb:
      call void @abort()
      unreachable
    }
  )");
  ASSERT_NE(module, nullptr);

  const Function *function = module->getFunction("f");
  ASSERT_NE(function, nullptr);
  const BasicBlock *loop = getBlock(*module, "f", "loop");
  const BasicBlock *abortbb = getBlock(*module, "f", "abortbb");
  ASSERT_NE(loop, nullptr);
  ASSERT_NE(abortbb, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(module.get());
  allocator.initDominatorsForFunction(function);
  allocator.initPostDominatorsForFunction(function);
  allocator.initLoopInfoForFunction(function);
  allocator.allocate();

  auto guard = allocator.evaluateLoopExitBranch(loop, abortbb);
  EXPECT_EQ(allocator.dumpCond(guard),
            allocator.dumpCond(SaberCondAllocator::Condition::nullExpr()));
}

TEST(SaberConditionAllocatorTest, SetModulePreservesImportedSourceSinkState) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define void @f() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  DummySrcSnkDDA checker;
  char src_storage = 0;
  char sink_storage = 0;
  char call_storage = 0;
  SrcSnkDDA::SVFGNodeSet sources = {
      reinterpret_cast<const SVFGNode *>(&src_storage)};
  SrcSnkDDA::SVFGNodeSet sinks = {
      reinterpret_cast<const SVFGNode *>(&sink_storage)};
  SrcSnkDDA::SrcToCSMap srcToCS = {
      {reinterpret_cast<const SVFGNode *>(&src_storage),
       reinterpret_cast<const CallBase *>(&call_storage)}};

  checker.importSourceSinkState(sources, sinks, srcToCS);
  checker.setModule(module.get());

  SrcSnkDDA::SVFGNodeSet exportedSources;
  SrcSnkDDA::SVFGNodeSet exportedSinks;
  SrcSnkDDA::SrcToCSMap exportedSrcToCS;
  checker.exportSourceSinkState(exportedSources, exportedSinks, exportedSrcToCS);

  EXPECT_EQ(exportedSources, sources);
  EXPECT_EQ(exportedSinks, sinks);
  EXPECT_EQ(exportedSrcToCS, srcToCS);
}

TEST(SaberConditionAllocatorTest,
     SharedGraphInitializationPreservesRemovedStrongUpdateEdges) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define void @f() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  DummySrcSnkDDA checker;
  checker.setSharedSVFGAndICFG(std::make_unique<SVFG>(),
                               std::make_unique<ICFG>());
  checker.setModule(module.get());

  char src_storage = 0;
  char sink_storage = 0;
  auto *src = reinterpret_cast<const SVFGNode *>(&src_storage);
  auto *sink = reinterpret_cast<const SVFGNode *>(&sink_storage);
  checker.getSaberCondAllocator()->getRemovedSUVFEdges()[src].insert(sink);

  checker.initialize();

  const auto &removed = checker.getSaberCondAllocator()->getRemovedSUVFEdges();
  ASSERT_EQ(removed.size(), 1u);
  auto it = removed.find(src);
  ASSERT_NE(it, removed.end());
  EXPECT_EQ(it->second.size(), 1u);
  EXPECT_EQ(*it->second.begin(), sink);
  EXPECT_TRUE(checker.hasSVFGAndICFG());
}
