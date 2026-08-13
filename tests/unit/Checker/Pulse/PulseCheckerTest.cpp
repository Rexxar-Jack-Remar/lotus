/**
 * @file PulseCheckerTest.cpp
 * @brief Behavioral tests for the Pulse checker.
 *
 * These tests focus on checker-visible outcomes:
 * - concrete bug reports emitted through BugReportMgr
 * - soundness-oriented regressions around latent issues and modeling
 * - summary and loop behavior that affects witness construction
 */

#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Domain/PulseDisjunctiveDomain.h"
#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

#include <iterator>

using namespace llvm;
using namespace pulse;
using lotus::unittest::findCallTo;
using lotus::unittest::parseModule;

namespace {

size_t getReportCountForType(BugReportMgr &mgr, StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0) {
    return 0;
  }
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  return reports ? reports->size() : 0;
}

const BugReport *getLastReportForType(BugReportMgr &mgr,
                                      StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0) {
    return nullptr;
  }
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  if (!reports || reports->empty()) {
    return nullptr;
  }
  return reports->back();
}

bool reportContainsTip(const BugReport *report, StringRef tipSubstring) {
  if (!report) {
    return false;
  }
  for (const BugDiagStep *step : report->get_steps()) {
    if (step && StringRef(step->tip).contains(tipSubstring)) {
      return true;
    }
  }
  return false;
}

const BugDiagStep *getLastStep(const BugReport *report) {
  if (!report || report->get_steps().empty()) {
    return nullptr;
  }
  return report->get_steps().back();
}

template <typename InstT>
InstT *findNthInstruction(Function *F, unsigned ordinal) {
  unsigned seen = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *inst = dyn_cast<InstT>(&I)) {
        if (seen == ordinal) {
          return inst;
        }
        ++seen;
      }
    }
  }
  return nullptr;
}

} // namespace

class PulseCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;

  void SetUp() override { DiagnosticManager::getInstance().clear(); }

  ExecutionDomain executeEntryBlock(PulseChecker &checker, Function *F,
                                    const Instruction *stop_after = nullptr) {
    ExecutionDomain state = checker.initializeFunction(F);
    for (auto &I : F->getEntryBlock()) {
      auto states =
          checker.executeInstruction(&I, std::move(state), nullptr, 0);
      EXPECT_FALSE(states.empty());
      if (states.empty()) {
        return ExecutionDomain();
      }
      state = std::move(states.front());
      if (&I == stop_after) {
        break;
      }
    }
    return state;
  }
};

TEST_F(PulseCheckerTest, ReportsNullDereferenceWithConcreteWitness) {
  auto module = parseModule(context, R"(
    define i8 @null_deref() {
    entry:
      %v = load i8, i8* null
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("null_deref");
  ASSERT_NE(F, nullptr);
  LoadInst *sink = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(sink, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::NullDereference);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::NullDereference);
  ASSERT_NE(report, nullptr);
  ASSERT_TRUE(reportContainsTip(report, "Null pointer dereference"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, sink);
}

TEST_F(PulseCheckerTest, ConstantFalseBranchDoesNotReportUnreachableFault) {
  auto module = parseModule(context, R"(
    define i32 @unreachable_null_dereference() {
    entry:
      br i1 false, label %bad, label %ok

    bad:
      %x = load i32, i32* null
      ret i32 %x

    ok:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::NullDereference);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);
}

TEST_F(PulseCheckerTest, LoadedFalseBranchDoesNotReportUnreachableFault) {
  auto module = parseModule(context, R"(
    define i32 @loaded_false_branch() {
    entry:
      %condition = alloca i1
      store i1 false, i1* %condition
      %loaded = load i1, i1* %condition
      br i1 %loaded, label %bad, label %ok

    bad:
      %x = load i32, i32* null
      ret i32 %x

    ok:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::NullDereference);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);
}

TEST_F(PulseCheckerTest, ConstantFalsePointerSelectUsesValidOperandOnly) {
  auto module = parseModule(context, R"(
    define i8 @constant_false_select() {
    entry:
      %valid = alloca i8
      store i8 7, i8* %valid
      %selected = select i1 false, i8* null, i8* %valid
      %value = load i8, i8* %selected
      ret i8 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::NullDereference);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);
}

TEST_F(PulseCheckerTest, ReportsUninitializedReadAtLoadSite) {
  auto module = parseModule(context, R"(
    define i32 @uninit_read() {
    entry:
      %x = alloca i32
      %v = load i32, i32* %x
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("uninit_read");
  ASSERT_NE(F, nullptr);
  LoadInst *load = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(load, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::UninitializedRead);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Load from uninitialized variable"));
  EXPECT_TRUE(reportContainsTip(report, "Reading uninitialized memory"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, load);
}

TEST_F(PulseCheckerTest, ReportsInvalidFreeOfStackPointer) {
  auto module = parseModule(context, R"(
    declare void @free(i8*)

    define void @bad_free() {
    entry:
      %stack = alloca i8
      call void @free(i8* %stack)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("bad_free");
  ASSERT_NE(F, nullptr);
  auto *freeCall = dyn_cast<CallInst>(findCallTo(F, "free"));
  ASSERT_NE(freeCall, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::InvalidFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::InvalidFree);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, IssueType::InvalidFree);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Invalid free: non-heap pointer"));
  EXPECT_TRUE(reportContainsTip(report, "Invalid free of stack memory"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, freeCall);
}

TEST_F(PulseCheckerTest, ReportsStackAddressEscapeViaReturn) {
  auto module = parseModule(context, R"(
    define i8* @return_local() {
    entry:
      %x = alloca i8
      ret i8* %x
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("return_local");
  ASSERT_NE(F, nullptr);
  auto *ret = dyn_cast<ReturnInst>(F->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(
      mgr, IssueType::StackVariableAddressEscape);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter = getReportCountForType(
      mgr, IssueType::StackVariableAddressEscape);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::StackVariableAddressEscape);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report,
                                "Returning address derived from stack "
                                "allocation"));
  EXPECT_TRUE(reportContainsTip(report, "Stack address escapes via return"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, ret);
}

TEST_F(PulseCheckerTest, LatentIssuesAreNotReportedAtShutdown) {
  auto module = parseModule(context, R"(
    define void @latent_branch() {
    entry:
      %p = alloca i8
      %cmp = icmp eq i8* %p, null
      br i1 %cmp, label %bad, label %ok

    bad:
      ret void

    ok:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const int reportsBefore = mgr.get_total_reports();
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  EXPECT_EQ(mgr.get_total_reports(), reportsBefore);
}

TEST_F(PulseCheckerTest, UnlockDoesNotInvalidateLockMemory) {
  auto module = parseModule(context, R"(
    declare i32 @pthread_mutex_unlock(i8*)

    define void @unlock_ok() {
    entry:
      %m = alloca i8
      %r = call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("unlock_ok");
  ASSERT_NE(F, nullptr);

  auto *allocaInst = dyn_cast<AllocaInst>(&*F->getEntryBlock().begin());
  ASSERT_NE(allocaInst, nullptr);
  auto *unlockCall = dyn_cast<CallInst>(&*std::next(F->getEntryBlock().begin()));
  ASSERT_NE(unlockCall, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = executeEntryBlock(checker, F, unlockCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue lockAddr = checker.getFactory().getOrCreate(allocaInst);
  EXPECT_FALSE(astate->getPostAttrs().has(lockAddr, pulse::Attribute::Invalid));
}

TEST_F(PulseCheckerTest, NonReallocAssignmentDoesNotInvalidatePreviousValue) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define void @assign(i8** %slot) {
    entry:
      %new = call i8* @malloc(i64 8)
      store i8* %new, i8** %slot
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("assign");
  ASSERT_NE(F, nullptr);
  Argument *slotArg = F->arg_empty() ? nullptr : &*F->arg_begin();
  ASSERT_NE(slotArg, nullptr);

  auto callIt = F->getEntryBlock().begin();
  auto *mallocCall = dyn_cast<CallInst>(&*callIt);
  ASSERT_NE(mallocCall, nullptr);
  auto *store = dyn_cast<StoreInst>(&*std::next(callIt));
  ASSERT_NE(store, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue oldValue = checker.getFactory().createFresh();
  checker.getOperations().allocate(*astate, oldValue, nullptr);
  astate->getPostStack().add(slotArg, Address(oldValue));
  astate->getPostAttrs().remove(oldValue, pulse::Attribute::Invalid);
  astate->getPostAttrs().remove(oldValue, pulse::Attribute::Uninitialized);

  auto mallocStates =
      checker.executeInstruction(mallocCall, std::move(state), nullptr, 0);
  ASSERT_EQ(mallocStates.size(), 1u);
  state = std::move(mallocStates.front());

  auto storeStates =
      checker.executeInstruction(store, std::move(state), nullptr, 0);
  ASSERT_EQ(storeStates.size(), 1u);
  astate = storeStates.front().getAstate();
  ASSERT_NE(astate, nullptr);

  EXPECT_FALSE(
      astate->getPostAttrs().has(oldValue, pulse::Attribute::Invalid));
}

TEST_F(PulseCheckerTest, SummaryKeepsReturnValueFromMatchingExitPath) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @choose_ret(i8* %p) {
    entry:
      %isnull = icmp eq i8* %p, null
      br i1 %isnull, label %ret_null, label %ret_arg

    ret_null:
      ret i8* null

    ret_arg:
      ret i8* %p
    }

    define void @caller() {
    entry:
      %p = call i8* @malloc(i64 1)
      %q = call i8* @choose_ret(i8* %p)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  auto *mallocCall = dyn_cast<CallInst>(findCallTo(caller, "malloc"));
  auto *chooseCall = dyn_cast<CallInst>(findCallTo(caller, "choose_ret"));
  ASSERT_NE(mallocCall, nullptr);
  ASSERT_NE(chooseCall, nullptr);

  PulseChecker checker(module.get());
  checker.analyze();

  ExecutionDomain state = executeEntryBlock(checker, caller, chooseCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  const Address *mallocAddr = astate->getPostStack().find(mallocCall);
  const Address *callAddr = astate->getPostStack().find(chooseCall);
  ASSERT_NE(mallocAddr, nullptr);
  ASSERT_NE(callAddr, nullptr);

  EXPECT_EQ(astate->getCanonical(mallocAddr->addr),
            astate->getCanonical(callAddr->addr));
  EXPECT_FALSE(
      astate->getPostAttrs().has(astate->getCanonical(callAddr->addr),
                                 pulse::Attribute::Null));
}

TEST_F(PulseCheckerTest, RejectedMultiEntrySummaryFallsBackToUnknownCall) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @pick_first(i8* %a, i8* %b, i1 %flag) {
    entry:
      %distinct = icmp ne i8* %a, %b
      br i1 %distinct, label %ok, label %reject

    ok:
      br i1 %flag, label %ret1, label %ret2

    ret1:
      ret i8* %a

    ret2:
      ret i8* %a

    reject:
      unreachable
    }

    define void @caller_same_ptr() {
    entry:
      %p = call i8* @malloc(i64 1)
      %q = call i8* @pick_first(i8* %p, i8* %p, i1 false)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *caller = module->getFunction("caller_same_ptr");
  ASSERT_NE(caller, nullptr);
  auto *pickCall = dyn_cast<CallInst>(findCallTo(caller, "pick_first"));
  ASSERT_NE(pickCall, nullptr);

  PulseChecker checker(module.get());
  checker.analyze();

  ExecutionDomain state = executeEntryBlock(checker, caller, pickCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  EXPECT_TRUE(astate->hasSkippedCall("pick_first"));
}

TEST_F(PulseCheckerTest, SummaryApplicationHandlesAliasedActuals) {
  auto module = parseModule(context, R"(
    define void @store_two(i8** %a, i8** %b, i8* %v) {
    entry:
      store i8* %v, i8** %a
      store i8* %v, i8** %b
      ret void
    }

    define void @caller(i8** %p, i8* %v) {
    entry:
      call void @store_two(i8** %p, i8** %p, i8* %v)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *callee = module->getFunction("store_two");
  auto *caller = module->getFunction("caller");
  ASSERT_NE(callee, nullptr);
  ASSERT_NE(caller, nullptr);

  PulseChecker checker(module.get());
  checker.analyzeFunction(callee);

  ExecutionDomain callerState = checker.initializeFunction(caller);
  auto *call = dyn_cast<CallInst>(findCallTo(caller, "store_two"));
  ASSERT_NE(call, nullptr);

  auto applied = checker.applySummaryImproved(callee, callerState, call, nullptr);
  ASSERT_FALSE(applied.empty());
}

TEST_F(PulseCheckerTest, UnknownCallHavocsPointerArguments) {
  auto module = parseModule(context, R"(
    declare void @mystery(i8**)

    define void @caller() {
    entry:
      %slot = alloca i8*
      %buf = alloca i8
      store i8* %buf, i8** %slot
      call void @mystery(i8** %slot)
      %p = load i8*, i8** %slot
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("caller");
  ASSERT_NE(F, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *call = dyn_cast<CallInst>(findCallTo(F, "mystery"));
  ASSERT_NE(call, nullptr);

  for (auto &I : F->getEntryBlock()) {
    auto states = checker.executeInstruction(&I, std::move(state), nullptr, 0);
    ASSERT_FALSE(states.empty());
    state = std::move(states.front());
    if (&I == call) {
      break;
    }
  }

  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  auto *slot = findNthInstruction<AllocaInst>(F, 0);
  ASSERT_NE(slot, nullptr);
  AbstractValue slot_av = checker.getFactory().getOrCreate(slot);
  EXPECT_EQ(astate->getPostHeap().findEdge(astate->getCanonical(slot_av),
                                           Access(AccessKind::Dereference)),
            nullptr);
}

TEST_F(PulseCheckerTest, TaintSourceAndSinkProduceReport) {
  auto module = parseModule(context, R"(
    declare i8* @getenv(i8*)
    declare i32 @system(i8*)

    define i32 @taint_flow(i8* %name) {
    entry:
      %v = call i8* @getenv(i8* %name)
      %r = call i32 @system(i8* %v)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, IssueType::TaintError);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  const size_t reportsAfter = getReportCountForType(mgr, IssueType::TaintError);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}

TEST_F(PulseCheckerTest, LoopConvergenceRequiresEquivalentPathStamp) {
  auto module = parseModule(context, R"(
    define void @test_loop(i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %next = add i32 %i, 1
      %cmp = icmp slt i32 %next, %n
      br i1 %cmp, label %loop, label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_loop");
  ASSERT_NE(F, nullptr);

  DominatorTree DT(*F);
  LoopInfo LI;
  LI.analyze(DT);

  LoopAbstraction loopAbs;
  loopAbs.initialize(LI);

  BasicBlock *header = nullptr;
  for (auto &BB : *F) {
    if (BB.getName() == "loop") {
      header = &BB;
      break;
    }
  }
  ASSERT_NE(header, nullptr);

  ExecutionDomain firstState;
  ExecutionDomain secondState;
  ExecutionDomain thirdState;

  AbstractValue a(nullptr, 1);
  AbstractValue b(nullptr, 2);
  ASSERT_TRUE(secondState.getAstate()->getPathFormula().addNull(a));
  ASSERT_TRUE(thirdState.getAstate()->getPathFormula().addNull(b));

  EXPECT_FALSE(loopAbs.visitHeader(header, firstState));
  EXPECT_TRUE(loopAbs.visitHeader(header, secondState));
  EXPECT_TRUE(loopAbs.visitHeader(header, thirdState));
  EXPECT_FALSE(loopAbs.hasPreviousIterationSamePathStamp(header));
}

TEST_F(PulseCheckerTest, ReallocAssignmentReportsUseAfterFree) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)

    define void @realloc_alias_uaf() {
    entry:
      %slot = alloca i8*
      %p0 = call i8* @malloc(i64 1)
      store i8* %p0, i8** %slot
      %alias = load i8*, i8** %slot
      %p1 = call i8* @realloc(i8* %p0, i64 2)
      store i8* %p1, i8** %slot
      %v = load i8, i8* %alias
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("realloc_alias_uaf");
  ASSERT_NE(F, nullptr);
  LoadInst *sink = findNthInstruction<LoadInst>(F, 1);
  ASSERT_NE(sink, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::UseAfterFree);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, IssueType::UseAfterFree);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Use after free detected"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, sink);
}

TEST_F(PulseCheckerTest, ComputingComparisonDoesNotAssumeItsResult) {
  auto module = parseModule(context, R"(
    define i1 @compare_only(i8* %a, i8* %b) {
    entry:
      %cmp = icmp eq i8* %a, %b
      ret i1 %cmp
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("compare_only");
  ASSERT_NE(F, nullptr);
  auto *cmp = findNthInstruction<ICmpInst>(F, 0);
  ASSERT_NE(cmp, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto states = checker.executeInstruction(cmp, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  auto *astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue a = checker.getFactory().getOrCreate(F->getArg(0));
  AbstractValue b = checker.getFactory().getOrCreate(F->getArg(1));
  EXPECT_FALSE(astate->getPathFormula().areEqual(a, b));
  EXPECT_FALSE(astate->getPathFormula().areDisequal(a, b));
}

TEST_F(PulseCheckerTest, PointerArgumentLoadAbducesPointee) {
  auto module = parseModule(context, R"(
    define i32 @load_arg(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("load_arg");
  ASSERT_NE(F, nullptr);
  auto *load = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(load, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto states = checker.executeInstruction(load, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  auto *astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue p = checker.getFactory().getOrCreate(F->getArg(0));
  Access deref(AccessKind::Dereference);
  const Address *postPointee =
      astate->getPostHeap().findEdge(astate->getCanonical(p), deref);
  const Address *prePointee =
      astate->getPreHeap().findEdge(astate->getCanonical(p), deref);
  const Address *loaded = astate->getPostStack().find(load);
  ASSERT_NE(postPointee, nullptr);
  ASSERT_NE(prePointee, nullptr);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(astate->getCanonical(postPointee->addr),
            astate->getCanonical(loaded->addr));
}

TEST_F(PulseCheckerTest, PointerArgumentStoreWritesPointee) {
  auto module = parseModule(context, R"(
    define void @store_arg(i32* %p) {
    entry:
      store i32 7, i32* %p
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("store_arg");
  ASSERT_NE(F, nullptr);
  auto *store = findNthInstruction<StoreInst>(F, 0);
  ASSERT_NE(store, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto states = checker.executeInstruction(store, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  auto *astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue p = checker.getFactory().getOrCreate(F->getArg(0));
  Access deref(AccessKind::Dereference);
  EXPECT_NE(astate->getPostHeap().findEdge(astate->getCanonical(p), deref),
            nullptr);
  EXPECT_EQ(astate->getPreHeap().findEdge(astate->getCanonical(p), deref),
            nullptr);
  EXPECT_TRUE(astate->getPreAttrs().has(astate->getCanonical(p),
                                        pulse::Attribute::Allocated));
  const Address *binding = astate->getPostStack().find(F->getArg(0));
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(astate->getCanonical(binding->addr), astate->getCanonical(p));
}

TEST_F(PulseCheckerTest, DisjunctReductionDoesNotKeepMovedFromStates) {
  auto module = parseModule(context, R"(
    define void @many_paths() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  BasicBlock *entry = &module->getFunction("many_paths")->getEntryBlock();

  DisjunctiveDomain domain;
  for (unsigned i = 0; i < 11; ++i) {
    domain.add(entry, ExecutionDomain(), entry);
  }

  const auto &disjuncts = domain.getDisjuncts(entry);
  ASSERT_EQ(disjuncts.size(), 10u);
  for (const auto &disjunct : disjuncts) {
    EXPECT_NE(disjunct.state.getAstate(), nullptr);
  }
}

TEST_F(PulseCheckerTest, SummaryStoreInitializesCallerPointee) {
  auto module = parseModule(context, R"(
    define void @init(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define void @caller() {
    entry:
      %x = alloca i32
      call void @init(i32* %x)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *init = module->getFunction("init");
  Function *caller = module->getFunction("caller");
  ASSERT_NE(init, nullptr);
  ASSERT_NE(caller, nullptr);
  auto *allocaInst = findNthInstruction<AllocaInst>(caller, 0);
  auto *call = dyn_cast<CallInst>(findCallTo(caller, "init"));
  ASSERT_NE(allocaInst, nullptr);
  ASSERT_NE(call, nullptr);

  PulseChecker checker(module.get());
  checker.analyzeFunction(init);
  ExecutionDomain state = checker.initializeFunction(caller);
  auto allocaStates =
      checker.executeInstruction(allocaInst, std::move(state), nullptr, 0);
  ASSERT_EQ(allocaStates.size(), 1u);
  auto callStates = checker.executeInstruction(
      call, std::move(allocaStates.front()), nullptr, 0);
  ASSERT_FALSE(callStates.empty());
  auto *astate = callStates.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue x = checker.getFactory().getOrCreate(allocaInst);
  EXPECT_FALSE(astate->getPostAttrs().has(astate->getCanonical(x),
                                          pulse::Attribute::Uninitialized));
  EXPECT_NE(astate->getPostHeap().findEdge(astate->getCanonical(x),
                                           Access(AccessKind::Dereference)),
            nullptr);
}

TEST_F(PulseCheckerTest, SummaryPreservesNestedMaterializedObjectMapping) {
  auto module = parseModule(context, R"(
    define void @free_nested(i8*** %root) {
    entry:
      ret void
    }

    define void @caller(i8*** %root) {
    entry:
      call void @free_nested(i8*** %root)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *freeNext = module->getFunction("free_nested");
  Function *caller = module->getFunction("caller");
  ASSERT_NE(freeNext, nullptr);
  ASSERT_NE(caller, nullptr);
  auto *freeNextCall = dyn_cast<CallInst>(findCallTo(caller, "free_nested"));
  ASSERT_NE(freeNextCall, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(caller);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue root = checker.getFactory().getOrCreate(caller->getArg(0));
  AbstractValue child = checker.getFactory().createFresh();
  AbstractValue next = checker.getFactory().createFresh();
  astate->getPostHeap().addEdge(root, Access(AccessKind::Dereference),
                                Address(child));
  astate->getPostHeap().addEdge(child, Access(AccessKind::Dereference),
                                Address(next));
  checker.getOperations().allocate(*astate, root, nullptr);
  checker.getOperations().allocate(*astate, child, nullptr);
  checker.getOperations().allocate(*astate, next, nullptr);

  AbstractValue formalRoot =
      checker.getFactory().getOrCreate(freeNext->getArg(0));
  AbstractValue formalChild = checker.getFactory().createFresh();
  AbstractValue formalNext = checker.getFactory().createFresh();

  auto summaryPre = std::make_unique<AbductiveDomain>();
  summaryPre->getPreStack().add(freeNext->getArg(0), Address(formalRoot));
  summaryPre->getPreHeap().addEdge(formalRoot,
                                   Access(AccessKind::Dereference),
                                   Address(formalChild));
  summaryPre->getPreHeap().addEdge(formalChild,
                                   Access(AccessKind::Dereference),
                                   Address(formalNext));
  summaryPre->getPreAttrs().add(formalRoot, pulse::Attribute::Allocated);
  summaryPre->getPreAttrs().add(formalChild, pulse::Attribute::Allocated);

  auto summaryPost = std::make_unique<AbductiveDomain>();
  summaryPost->getPostStack().add(freeNext->getArg(0), Address(formalRoot));
  summaryPost->getPostHeap().addEdge(formalRoot,
                                     Access(AccessKind::Dereference),
                                     Address(formalChild));
  summaryPost->getPostHeap().addEdge(formalChild,
                                     Access(AccessKind::Dereference),
                                     Address(formalNext));
  summaryPost->getPostAttrs().add(formalNext, pulse::Attribute::Invalid);

  PulseSummary summary(freeNext);
  summary.setFormalAV(freeNext->getArg(0), formalRoot);
  summary.addPrePost(SummaryEntry(
      std::move(summaryPre), PulseFormula(), std::move(summaryPost),
      PulseFormula(), std::nullopt));

  auto states = checker.applySummaryImproved(
      freeNext, state, freeNextCall, nullptr, &summary);
  ASSERT_FALSE(states.empty());
  astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);
  EXPECT_TRUE(astate->getPostAttrs().has(astate->getCanonical(next),
                                         pulse::Attribute::Invalid));
}

TEST_F(PulseCheckerTest, NullFormalBranchDereferenceIsReported) {
  auto module = parseModule(context, R"(
    define i8 @null_formal(i8* %p) {
    entry:
      %isnull = icmp eq i8* %p, null
      br i1 %isnull, label %bad, label %ok

    bad:
      %v = load i8, i8* %p
      ret i8 %v

    ok:
      ret i8 0
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("null_formal");
  ASSERT_NE(F, nullptr);
  auto *load = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(load, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  AbstractValue p = checker.getFactory().getOrCreate(F->getArg(0));
  ASSERT_TRUE(astate->getPathFormula().addNull(p));

  auto states = checker.executeInstruction(load, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  ASSERT_TRUE(states.front().isStopped());
  const auto &stopped = states.front().getStoppedExecution();
  EXPECT_EQ(stopped.diagnostic, OperationResult::NullDereference);
}

TEST_F(PulseCheckerTest, SwitchDropsInfeasibleCaseWitness) {
  auto module = parseModule(context, R"(
    define void @infeasible_switch(i32 %x) {
    entry:
      %not_one = icmp ne i32 %x, 1
      br i1 %not_one, label %dispatch, label %exit

    dispatch:
      switch i32 %x, label %exit [
        i32 1, label %bad
      ]

    bad:
      %v = load i8, i8* null
      ret void

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::NullDereference);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);
}

TEST_F(PulseCheckerTest, WideningKeepsPhiPredecessorWithRepresentativeState) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @safe_three_way_merge(i32 %choice) {
    entry:
      %q = call i8* @malloc(i64 1)
      %r = call i8* @malloc(i64 1)
      switch i32 %choice, label %third [
        i32 0, label %first
        i32 1, label %second
      ]

    first:
      call void @free(i8* %q)
      br label %merge

    second:
      br label %merge

    third:
      br label %merge

    merge:
      %selected = phi i8* [ %r, %first ], [ %r, %second ], [ %q, %third ]
      %value = load i8, i8* %selected
      ret i8 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before);
}

TEST_F(PulseCheckerTest, SymbolicOutOfBoundsBranchIsReported) {
  auto module = parseModule(context, R"(
    define i32 @symbolic_oob(i64 %i) {
    entry:
      %a = alloca [2 x i32]
      %bad = icmp sge i64 %i, 2
      br i1 %bad, label %oob, label %ok

    oob:
      %p = getelementptr [2 x i32], [2 x i32]* %a, i64 0, i64 %i
      %v = load i32, i32* %p
      ret i32 %v

    ok:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::OutOfBounds);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::OutOfBounds), before + 1);
}

TEST_F(PulseCheckerTest, PartialArrayInitializationDoesNotInitializeAllElements) {
  auto module = parseModule(context, R"(
    define i32 @partial_init() {
    entry:
      %a = alloca [2 x i32]
      %p0 = getelementptr [2 x i32], [2 x i32]* %a, i64 0, i64 0
      store i32 1, i32* %p0
      %p1 = getelementptr [2 x i32], [2 x i32]* %a, i64 0, i64 1
      %v = load i32, i32* %p1
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UninitializedRead),
            before + 1);
}

TEST_F(PulseCheckerTest, ReadTaintsDestinationBuffer) {
  auto module = parseModule(context, R"(
    declare i64 @read(i32, i8*, i64)
    declare i32 @system(i8*)

    define void @read_to_system(i32 %fd) {
    entry:
      %buf = alloca [8 x i8]
      %p = bitcast [8 x i8]* %buf to i8*
      %n = call i64 @read(i32 %fd, i8* %p, i64 8)
      %r = call i32 @system(i8* %p)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  PulseChecker checker(module.get());
  Function *F = module->getFunction("read_to_system");
  ASSERT_NE(F, nullptr);
  auto *readCall = dyn_cast<CallInst>(findCallTo(F, "read"));
  auto *systemCall = dyn_cast<CallInst>(findCallTo(F, "system"));
  ASSERT_NE(readCall, nullptr);
  ASSERT_NE(systemCall, nullptr);
  ExecutionDomain state = checker.initializeFunction(F);
  for (auto &I : F->getEntryBlock()) {
    auto states = checker.executeInstruction(&I, std::move(state), nullptr, 0);
    ASSERT_FALSE(states.empty());
    state = std::move(states.front());
    if (&I == readCall)
      break;
  }
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  auto bufOpt = checker.getOperations().eval(
      *astate, readCall->getArgOperand(1), readCall, nullptr);
  ASSERT_TRUE(bufOpt.has_value());
  EXPECT_TRUE(astate->getTaintDomain().has(
      astate->getCanonical(bufOpt->addr)));
  EXPECT_TRUE(TaintOperations::checkSink(
      *astate, astate->getCanonical(bufOpt->addr), "system", systemCall));
}

TEST_F(PulseCheckerTest, FreeInvalidatesDerivedPointer) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @derived_uaf() {
    entry:
      %p = call i8* @malloc(i64 4)
      %q = getelementptr i8, i8* %p, i64 1
      call void @free(i8* %p)
      %v = load i8, i8* %q
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before + 1);
}

TEST_F(PulseCheckerTest, IndirectCallIsNotModeledAsNoOp) {
  auto module = parseModule(context, R"(
    define void @indirect(void (i8**)* %fp, i8** %slot) {
    entry:
      call void %fp(i8** %slot)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("indirect");
  ASSERT_NE(F, nullptr);
  auto *call = findNthInstruction<CallInst>(F, 0);
  ASSERT_NE(call, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  Argument *slot = F->getArg(1);
  AbstractValue slotAv = checker.getFactory().getOrCreate(slot);
  AbstractValue pointee = checker.getFactory().createFresh();
  astate->getPostHeap().addEdge(slotAv, Access(AccessKind::Dereference),
                                Address(pointee));

  auto states = checker.executeInstruction(call, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);
  EXPECT_TRUE(astate->hasUnknownValues());
  EXPECT_TRUE(astate->hasSkippedCall("<indirect>"));
  EXPECT_EQ(astate->getPostHeap().findEdge(
                astate->getCanonical(slotAv), Access(AccessKind::Dereference)),
            nullptr);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
