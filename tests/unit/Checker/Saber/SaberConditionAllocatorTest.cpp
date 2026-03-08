#include "Checker/Saber/SaberCondAllocator.h"
#include "Checker/Saber/DoubleFreeChecker.h"
#include "Checker/Saber/FileChecker.h"
#include "Checker/Saber/LeakChecker.h"
#include "Checker/Saber/SaberSVFGBuilder.h"
#include "Checker/Saber/SrcSnkDDA.h"
#include "Checker/Report/BugReportMgr.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"

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

const CallBase *getOnlyCall(const Module &module, StringRef functionName) {
  const Function *function = module.getFunction(functionName);
  if (!function)
    return nullptr;
  const CallBase *call = nullptr;
  for (const BasicBlock &bb : *function) {
    for (const Instruction &inst : bb) {
      if (const auto *cb = dyn_cast<CallBase>(&inst)) {
        if (call)
          return nullptr;
        call = cb;
      }
    }
  }
  return call;
}

const StoreInst *getOnlyStore(const Module &module, StringRef functionName) {
  const Function *function = module.getFunction(functionName);
  if (!function)
    return nullptr;
  const StoreInst *store = nullptr;
  for (const BasicBlock &bb : *function) {
    for (const Instruction &inst : bb) {
      if (const auto *si = dyn_cast<StoreInst>(&inst)) {
        if (store)
          return nullptr;
        store = si;
      }
    }
  }
  return store;
}

size_t getReportCountForType(BugReportMgr &mgr, StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0)
    return 0;
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  return reports ? reports->size() : 0;
}

const BugReport *getLastReportForType(BugReportMgr &mgr, StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0)
    return nullptr;
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  if (!reports || reports->empty())
    return nullptr;
  return reports->back();
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

class TestSaberSVFGBuilder final : public SaberSVFGBuilder {
public:
  bool isStrongUpdatePublic(const SVFGNode *node, uint32_t &singleton) {
    return isStrongUpdate(node, singleton);
  }
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
        i32 2, label %case2
      ]
    case0:
      ret i32 0
    case1:
      ret i32 1
    case2:
      ret i32 2
    default:
      ret i32 3
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

  ASSERT_EQ(guards.size(), 4u);
  EXPECT_TRUE(
      allocator.isEquivalentBranchCond(disjunction, allocator.getTrueCond()));
  for (size_t i = 0; i < guards.size(); ++i) {
    for (size_t j = i + 1; j < guards.size(); ++j) {
      EXPECT_TRUE(allocator.isEquivalentBranchCond(
          allocator.condAnd(guards[i], guards[j]), allocator.getFalseCond()));
    }
  }
}

TEST(SaberConditionAllocatorTest, FourWayBranchUsesCeilLog2DecisionVariables) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @f(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 0, label %case0
        i32 1, label %case1
        i32 2, label %case2
      ]
    case0:
      ret i32 0
    case1:
      ret i32 1
    case2:
      ret i32 2
    default:
      ret i32 3
    }
  )");
  ASSERT_NE(module, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(module.get());
  allocator.allocate();

  EXPECT_EQ(allocator.getCondNum(), 2u);
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

TEST(SaberConditionAllocatorTest,
     InitializeResolvesIndirectCallsForSourceSinkTraversal) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define i8* @target() {
    entry:
      ret i8* null
    }

    define i8* @caller(i8* ()* %fp) {
    entry:
      %r = call i8* %fp()
      ret i8* %r
    }

    define i32 @main() {
    entry:
      %r = call i8* @caller(i8* ()* @target)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  DummySrcSnkDDA checker;
  checker.setModule(module.get());
  checker.initialize();

  const CallBase *indirectCall = getOnlyCall(*module, "caller");
  const Function *target = module->getFunction("target");
  ASSERT_NE(indirectCall, nullptr);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(checker.getSVFG(), nullptr);

  EXPECT_NE(checker.getSVFG()->getCallSiteId(indirectCall, target), 0u);
}

TEST(SaberConditionAllocatorTest,
     LeakCheckerAddsExtraLoadSinkOnlyForMultiLevelFreeApis) {
  LLVMContext context;
  auto xfreeModule = parseModule(context, R"(
    declare void @XFree(i8**)

    define void @test() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %loaded = load i8*, i8** %slot
      call void @XFree(i8** %slot)
      ret void
    }

    define i32 @main() {
    entry:
      call void @test()
      ret i32 0
    }
  )");
  auto freeModule = parseModule(context, R"(
    declare void @free(i8**)

    define void @test() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %loaded = load i8*, i8** %slot
      call void @free(i8** %slot)
      ret void
    }

    define i32 @main() {
    entry:
      call void @test()
      ret i32 0
    }
  )");
  ASSERT_NE(xfreeModule, nullptr);
  ASSERT_NE(freeModule, nullptr);

  LeakChecker xfreeChecker;
  xfreeChecker.setModule(xfreeModule.get());
  xfreeChecker.initialize();
  EXPECT_EQ(xfreeChecker.getSinks().size(), 2u);

  LeakChecker freeChecker;
  freeChecker.setModule(freeModule.get());
  freeChecker.initialize();
  EXPECT_EQ(freeChecker.getSinks().size(), 1u);
}

TEST(SaberConditionAllocatorTest,
     StrongUpdateIsDisabledForIndirectRecursiveStackObjects) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    @fp = global void ()* @f

    define void @f() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %callee = load void ()*, void ()** @fp
      call void %callee()
      ret void
    }

    define i32 @main() {
    entry:
      call void @f()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  auto icfg = std::make_unique<ICFG>();
  ICFGBuilder icfgBuilder(icfg.get());
  icfgBuilder.build(module.get());

  TestSaberSVFGBuilder builder;
  builder.setModule(module.get());
  SVFG *svfg = builder.buildSVFG(icfg.get());
  ASSERT_NE(svfg, nullptr);

  const StoreInst *store = getOnlyStore(*module, "f");
  ASSERT_NE(store, nullptr);
  SVFGNode *storeNode = svfg->getDef(store);
  ASSERT_NE(storeNode, nullptr);

  uint32_t singleton = 0;
  EXPECT_FALSE(builder.isStrongUpdatePublic(storeNode, singleton));
}

TEST(SaberConditionAllocatorTest, LeakCheckerReportsCallsiteAsSourceStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @cleanup() {
    entry:
      call void @free(i8* null)
      ret void
    }

    define void @leak() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Memory Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());

  const BugDiagStep *sourceStep = report->get_steps().front();
  ASSERT_NE(sourceStep, nullptr);
  EXPECT_EQ(sourceStep->tip, "Memory allocated here is never freed");
  const auto *call = dyn_cast_or_null<CallBase>(sourceStep->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "malloc");
}

TEST(SaberConditionAllocatorTest,
     DoubleFreeCheckerReportsAllocationCallsiteAsSourceStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @df() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      call void @df()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Double Free");

  DoubleFreeChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Double Free");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Double Free");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());

  const BugDiagStep *sourceStep = report->get_steps().front();
  ASSERT_NE(sourceStep, nullptr);
  EXPECT_EQ(sourceStep->tip, "Memory allocated here");
  const auto *call = dyn_cast_or_null<CallBase>(sourceStep->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "malloc");

  bool sawFreeStep = false;
  for (const BugDiagStep *step : report->get_steps()) {
    if (step && step->tip == "Memory deallocated along double-free path") {
      sawFreeStep = true;
      break;
    }
  }
  EXPECT_TRUE(sawFreeStep);
}

TEST(SaberConditionAllocatorTest, FileCheckerReportsFopenCallsiteAsSourceStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    %struct._IO_FILE = type opaque
    @path = private unnamed_addr constant [9 x i8] c"test.txt\00"
    @mode = private unnamed_addr constant [2 x i8] c"r\00"

    declare %struct._IO_FILE* @fopen(i8*, i8*)
    declare i32 @fclose(%struct._IO_FILE*)

    define void @cleanup() {
    entry:
      call i32 @fclose(%struct._IO_FILE* null)
      ret void
    }

    define void @leak_file() {
    entry:
      %path.ptr = getelementptr inbounds [9 x i8], [9 x i8]* @path, i64 0, i64 0
      %mode.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @mode, i64 0, i64 0
      %fp = call %struct._IO_FILE* @fopen(i8* %path.ptr, i8* %mode.ptr)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak_file()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "File Descriptor Leak");

  FileChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "File Descriptor Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "File Descriptor Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());

  const BugDiagStep *sourceStep = report->get_steps().front();
  ASSERT_NE(sourceStep, nullptr);
  EXPECT_EQ(sourceStep->tip, "File opened here");
  const auto *call = dyn_cast_or_null<CallBase>(sourceStep->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "fopen");
}

TEST(SaberConditionAllocatorTest,
     LeakCheckerDetectsBitcastedDirectAllocatorCalls) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @cleanup() {
    entry:
      call void @free(i8* null)
      ret void
    }

    define void @leak() {
    entry:
      %p = call i8* (i64, ...) bitcast (i8* (i64)* @malloc to i8* (i64, ...)*)(i64 4)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}

TEST(SaberConditionAllocatorTest,
     DoubleFreeCheckerDetectsBitcastedDirectDeallocatorCalls) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @df() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void (i8*, ...) bitcast (void (i8*)* @free to void (i8*, ...)*)(i8* %p)
      call void (i8*, ...) bitcast (void (i8*)* @free to void (i8*, ...)*)(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      call void @df()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Double Free");

  DoubleFreeChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Double Free");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}

TEST(SaberConditionAllocatorTest,
     FileCheckerDetectsBitcastedDirectFopenCalls) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    %struct._IO_FILE = type opaque
    @path = private unnamed_addr constant [9 x i8] c"test.txt\00"
    @mode = private unnamed_addr constant [2 x i8] c"r\00"

    declare %struct._IO_FILE* @fopen(i8*, i8*)
    declare i32 @fclose(%struct._IO_FILE*)

    define void @cleanup() {
    entry:
      call i32 @fclose(%struct._IO_FILE* null)
      ret void
    }

    define void @leak_file() {
    entry:
      %path.ptr = getelementptr inbounds [9 x i8], [9 x i8]* @path, i64 0, i64 0
      %mode.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @mode, i64 0, i64 0
      %fp = call %struct._IO_FILE* (i8*, i8*, ...) bitcast (%struct._IO_FILE* (i8*, i8*)* @fopen to %struct._IO_FILE* (i8*, i8*, ...)*)(i8* %path.ptr, i8* %mode.ptr)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak_file()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, "File Descriptor Leak");

  FileChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter =
      getReportCountForType(mgr, "File Descriptor Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}
