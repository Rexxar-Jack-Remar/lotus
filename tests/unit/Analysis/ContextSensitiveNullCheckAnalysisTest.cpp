#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

namespace {

using Context = ContextSensitiveNullCheckAnalysis::Context;

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &Context,
                                          const char *Source) {
  llvm::SMDiagnostic Err;
  auto Module = llvm::parseAssemblyString(Source, Err, Context);
  if (!Module) {
    Err.print("ContextSensitiveNullCheckAnalysisTest", llvm::errs());
  }
  return Module;
}

llvm::Instruction *findInstructionByName(llvm::Function *Function,
                                         llvm::StringRef Name) {
  for (auto &BB : *Function) {
    for (auto &Inst : BB) {
      if (Inst.getName() == Name) {
        return &Inst;
      }
    }
  }
  return nullptr;
}

std::vector<llvm::CallBase *> findCallsTo(llvm::Function *Function,
                                          llvm::StringRef CalleeName) {
  std::vector<llvm::CallBase *> Calls;
  for (auto &BB : *Function) {
    for (auto &Inst : BB) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (!Call || !Call->getCalledFunction()) {
        continue;
      }
      if (Call->getCalledFunction()->getName() == CalleeName) {
        Calls.push_back(Call);
      }
    }
  }
  return Calls;
}

bool contextEquals(const Context &Ctx,
                   std::initializer_list<llvm::CallBase *> Expected) {
  const auto &Elements = Ctx.elements();
  if (Elements.size() != Expected.size()) {
    return false;
  }
  auto It = Elements.begin();
  for (auto *Call : Expected) {
    if (*It++ != Call) {
      return false;
    }
  }
  return true;
}

struct AnalysisHarness {
  std::unique_ptr<llvm::legacy::PassManager> PassManager;
  ContextSensitiveNullCheckAnalysis *Analysis = nullptr;
};

AnalysisHarness runContextSensitiveNCA(llvm::Module &Module) {
  AnalysisHarness Harness;
  Harness.PassManager = std::make_unique<llvm::legacy::PassManager>();
  Harness.PassManager->add(new DyckAliasAnalysis());
  Harness.PassManager->add(new DyckModRefAnalysis());
  Harness.Analysis = new ContextSensitiveNullCheckAnalysis();
  Harness.PassManager->add(Harness.Analysis);
  Harness.PassManager->run(Module);
  return Harness;
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     DistinguishesExactCallerContextsButKeepsWholeFunctionQueriesConservative) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define void @callee(i8* %p) {
    entry:
      %load = load i8, i8* %p, align 1
      ret void
    }

    define void @caller_nonnull() {
    entry:
      %stack = alloca i8, align 1
      call void @callee(i8* %stack)
      ret void
    }

    define void @caller_nullable(i8* %arg) {
    entry:
      call void @callee(i8* %arg)
      ret void
    }

    define i32 @main() {
    entry:
      call void @caller_nonnull()
      call void @caller_nullable(i8* null)
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Callee = Module->getFunction("callee");
  auto *NonNullCaller = Module->getFunction("caller_nonnull");
  auto *NullableCaller = Module->getFunction("caller_nullable");
  auto *Load = findInstructionByName(Callee, "load");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(NonNullCaller, nullptr);
  ASSERT_NE(NullableCaller, nullptr);
  ASSERT_NE(Load, nullptr);

  auto *Formal = Callee->getArg(0);
  auto MainToNonNull = findCallsTo(Module->getFunction("main"), "caller_nonnull");
  auto MainToNullable =
      findCallsTo(Module->getFunction("main"), "caller_nullable");
  auto NonNullToCallee = findCallsTo(NonNullCaller, "callee");
  auto NullableToCallee = findCallsTo(NullableCaller, "callee");
  ASSERT_EQ(MainToNonNull.size(), 1u);
  ASSERT_EQ(MainToNullable.size(), 1u);
  ASSERT_EQ(NonNullToCallee.size(), 1u);
  ASSERT_EQ(NullableToCallee.size(), 1u);

  auto Reachable = Analysis->getReachableContexts(Load);
  ASSERT_GE(Reachable.size(), 2u);

  const Context *ExactNonNullCtx = nullptr;
  const Context *ExactNullableCtx = nullptr;
  for (const auto &Ctx : Reachable) {
    if (contextEquals(Ctx, {MainToNonNull.front(), NonNullToCallee.front()})) {
      ExactNonNullCtx = &Ctx;
    }
    if (contextEquals(Ctx, {MainToNullable.front(), NullableToCallee.front()})) {
      ExactNullableCtx = &Ctx;
    }
  }

  ASSERT_NE(ExactNonNullCtx, nullptr);
  ASSERT_NE(ExactNullableCtx, nullptr);
  EXPECT_FALSE(Analysis->mayNull(Formal, Load, *ExactNonNullCtx));
  EXPECT_TRUE(Analysis->mayNull(Formal, Load, *ExactNullableCtx));
  EXPECT_TRUE(Analysis->mayNull(Formal, Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     ResolvesIndirectCallsAndTracksMultipleContextsConservatively) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define void @callee(i8* %p) {
    entry:
      %load = load i8, i8* %p, align 1
      ret void
    }

    define void @dispatch(void (i8*)* %fp, i8* %p) {
    entry:
      call void %fp(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      call void @dispatch(void (i8*)* @callee, i8* %stack)
      call void @dispatch(void (i8*)* @callee, i8* null)
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Callee = Module->getFunction("callee");
  auto *Dispatch = Module->getFunction("dispatch");
  auto *Load = findInstructionByName(Callee, "load");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(Dispatch, nullptr);
  ASSERT_NE(Load, nullptr);

  llvm::CallBase *IndirectCall = nullptr;
  for (auto &BB : *Dispatch) {
    for (auto &Inst : BB) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (Call && !Call->getCalledFunction()) {
        IndirectCall = Call;
      }
    }
  }
  ASSERT_NE(IndirectCall, nullptr);

  auto MainToDispatch = findCallsTo(Module->getFunction("main"), "dispatch");
  ASSERT_EQ(MainToDispatch.size(), 2u);

  auto Reachable = Analysis->getReachableContexts(Load);
  EXPECT_GE(Reachable.size(), 2u);
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load));

  const Context *ExactNonNullCtx = nullptr;
  const Context *ExactNullableCtx = nullptr;
  for (const auto &Ctx : Reachable) {
    if (contextEquals(Ctx, {MainToDispatch[0], IndirectCall})) {
      ExactNonNullCtx = &Ctx;
    }
    if (contextEquals(Ctx, {MainToDispatch[1], IndirectCall})) {
      ExactNullableCtx = &Ctx;
    }
  }

  ASSERT_NE(ExactNonNullCtx, nullptr);
  ASSERT_NE(ExactNullableCtx, nullptr);
  EXPECT_FALSE(Analysis->mayNull(Callee->getArg(0), Load, *ExactNonNullCtx));
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load, *ExactNullableCtx));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     KLimitingDoesNotProduceFalseNotNullWhenOlderPrefixesDiffer) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define void @callee(i8* %p) {
    entry:
      %load = load i8, i8* %p, align 1
      ret void
    }

    define void @leaf_nonnull(i8* %p) {
    entry:
      call void @callee(i8* %p)
      ret void
    }

    define void @leaf_nullable(i8* %p) {
    entry:
      call void @callee(i8* %p)
      ret void
    }

    define void @mid_a_nonnull(i8* %p) {
    entry:
      call void @leaf_nonnull(i8* %p)
      ret void
    }

    define void @mid_a_nullable(i8* %p) {
    entry:
      call void @leaf_nullable(i8* %p)
      ret void
    }

    define void @mid_b_nonnull(i8* %p) {
    entry:
      call void @mid_a_nonnull(i8* %p)
      ret void
    }

    define void @mid_b_nullable(i8* %p) {
    entry:
      call void @mid_a_nullable(i8* %p)
      ret void
    }

    define void @root_nonnull() {
    entry:
      %stack = alloca i8, align 1
      call void @mid_b_nonnull(i8* %stack)
      ret void
    }

    define void @root_nullable(i8* %p) {
    entry:
      call void @mid_b_nullable(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      call void @root_nonnull()
      call void @root_nullable(i8* null)
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Callee = Module->getFunction("callee");
  auto *Load = findInstructionByName(Callee, "load");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(Load, nullptr);

  auto Reachable = Analysis->getReachableContexts(Load);
  ASSERT_FALSE(Reachable.empty());
  bool SawCollapsedMayNull = false;
  for (const auto &Ctx : Reachable) {
    if (Ctx.size() == 3 && Analysis->mayNull(Callee->getArg(0), Load, Ctx)) {
      SawCollapsedMayNull = true;
    }
  }
  EXPECT_TRUE(SawCollapsedMayNull);
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     MatchesReturnedNonNullFactsToTheCorrectCallSite) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @identity(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      %nonnull = call i8* @identity(i8* %stack)
      %nullable = call i8* @identity(i8* null)
      %load_nonnull = load i8, i8* %nonnull, align 1
      %load_nullable = load i8, i8* %nullable, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *NonNullRet = findInstructionByName(Main, "nonnull");
  auto *NullableRet = findInstructionByName(Main, "nullable");
  auto *LoadNonNull = findInstructionByName(Main, "load_nonnull");
  auto *LoadNullable = findInstructionByName(Main, "load_nullable");
  ASSERT_NE(NonNullRet, nullptr);
  ASSERT_NE(NullableRet, nullptr);
  ASSERT_NE(LoadNonNull, nullptr);
  ASSERT_NE(LoadNullable, nullptr);

  EXPECT_FALSE(Analysis->mayNull(NonNullRet, LoadNonNull));
  EXPECT_TRUE(Analysis->mayNull(NullableRet, LoadNullable));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     RecursiveReturnPropagationReachesAFixpoint) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @recur(i8* %p, i1 %stop) {
    entry:
      br i1 %stop, label %base, label %step
    base:
      ret i8* %p
    step:
      %next = call i8* @recur(i8* %p, i1 true)
      ret i8* %next
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      %ret = call i8* @recur(i8* %stack, i1 false)
      %load = load i8, i8* %ret, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *Ret = findInstructionByName(Main, "ret");
  auto *Load = findInstructionByName(Main, "load");
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Load, nullptr);

  EXPECT_FALSE(Analysis->mayNull(Ret, Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     OnlyStackAllocasAndNonnullContractsSeedGuaranteedNonNullFacts) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    declare i8* @malloc(i64)

    define i32 @main() {
    entry:
      %heap = call i8* @malloc(i64 4)
      %stack = alloca i8, align 1
      %heap_load = load i8, i8* %heap, align 1
      %stack_load = load i8, i8* %stack, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *Heap = findInstructionByName(Main, "heap");
  auto *Stack = findInstructionByName(Main, "stack");
  auto *HeapLoad = findInstructionByName(Main, "heap_load");
  auto *StackLoad = findInstructionByName(Main, "stack_load");
  ASSERT_NE(Heap, nullptr);
  ASSERT_NE(Stack, nullptr);
  ASSERT_NE(HeapLoad, nullptr);
  ASSERT_NE(StackLoad, nullptr);

  EXPECT_TRUE(Analysis->mayNull(Heap, HeapLoad));
  EXPECT_FALSE(Analysis->mayNull(Stack, StackLoad));
}

} // namespace
