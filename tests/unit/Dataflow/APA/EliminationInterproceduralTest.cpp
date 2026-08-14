#include "EliminationTestSupport.h"

TEST_F(APATest, InterproceduralReachabilityDirectCallAndDeadFunction) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      %sum = add i32 %x, 0
      ret i32 %sum
    }

    define i32 @dead() {
    entry:
      %unused = add i32 7, 8
      ret i32 %unused
    }

    define i32 @main() {
    entry:
      %call = call i32 @id(i32 3)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  auto *Dead = Module->getFunction("dead");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);
  ASSERT_NE(Dead, nullptr);

  auto Result = elimination::runInterElimReachable(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeEntry = &*Id->getEntryBlock().begin();
  auto *CalleeRet = findFirst<llvm::ReturnInst>(Id);
  auto *DeadInst = findInstructionByName(Dead, "unused");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);
  ASSERT_NE(CalleeRet, nullptr);
  ASSERT_NE(DeadInst, nullptr);

  EXPECT_NE(Result.tryIN(Call, {}), nullptr);
  EXPECT_NE(Result.tryIN(MainRet, {}), nullptr);
  EXPECT_NE(Result.tryIN(CalleeEntry, {Call}), nullptr);
  EXPECT_NE(Result.tryIN(CalleeRet, {Call}), nullptr);
  EXPECT_EQ(Result.tryIN(DeadInst, {}), nullptr);
}
TEST_F(APATest, ForwardSummaryReachabilityMatchesDirectCallWorklist) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      %sum = add i32 %x, 0
      ret i32 %sum
    }

    define i32 @dead() {
    entry:
      %unused = add i32 7, 8
      ret i32 %unused
    }

    define i32 @main() {
    entry:
      %call = call i32 @id(i32 3)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  auto *Dead = Module->getFunction("dead");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);
  ASSERT_NE(Dead, nullptr);

  auto Worklist = elimination::runInterElimReachable(Main);
  auto Summary = elimination::runInterSummaryElimReachable(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);
  ASSERT_TRUE(Summary.hasSummarySolveDiagnostics());
  EXPECT_GT(Summary.summarySolveDiagnostics().equation_node_count, 0u);
  EXPECT_GT(Summary.summarySolveDiagnostics().equation_edge_count, 0u);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeEntry = &*Id->getEntryBlock().begin();
  auto *CalleeRet = findFirst<llvm::ReturnInst>(Id);
  auto *DeadInst = findInstructionByName(Dead, "unused");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);
  ASSERT_NE(CalleeRet, nullptr);
  ASSERT_NE(DeadInst, nullptr);

  EXPECT_EQ(Worklist.tryIN(Call, {}) != nullptr,
            Summary.tryIN(Call, {}) != nullptr);
  EXPECT_EQ(Worklist.tryIN(MainRet, {}) != nullptr,
            Summary.tryIN(MainRet, {}) != nullptr);
  EXPECT_EQ(Worklist.tryIN(CalleeEntry, {Call}) != nullptr,
            Summary.tryIN(CalleeEntry, {Call}) != nullptr);
  EXPECT_EQ(Worklist.tryIN(CalleeRet, {Call}) != nullptr,
            Summary.tryIN(CalleeRet, {Call}) != nullptr);
  EXPECT_EQ(Summary.tryIN(DeadInst, {}), nullptr);
}
TEST_F(APATest, ForwardSummaryReachabilityHandlesRecursiveCallSCC) {
  const char *Source = R"(
    define i32 @rec(i32 %x) {
    entry:
      %iszero = icmp eq i32 %x, 0
      br i1 %iszero, label %base, label %step
    step:
      %next = sub i32 %x, 1
      %again = call i32 @rec(i32 %next)
      ret i32 %again
    base:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %call = call i32 @rec(i32 2)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Rec = Module->getFunction("rec");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Rec, nullptr);

  auto Summary = elimination::runInterSummaryElimReachable(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);
  ASSERT_TRUE(Summary.hasSummarySolveDiagnostics());
  EXPECT_GT(Summary.summarySolveDiagnostics().cyclic_scc_count, 0u);

  auto *MainCall = findFirst<llvm::CallInst>(Main);
  auto *RecCall = findFirst<llvm::CallInst>(Rec);
  auto *RecEntry = &*Rec->getEntryBlock().begin();
  ASSERT_NE(MainCall, nullptr);
  ASSERT_NE(RecCall, nullptr);
  ASSERT_NE(RecEntry, nullptr);

  EXPECT_NE(Summary.tryIN(RecEntry, {MainCall}), nullptr);
  EXPECT_NE(Summary.tryIN(RecEntry, {MainCall, RecCall}), nullptr);
}
TEST_F(APATest, InterproceduralConstantPropagationReturnThroughIdentity) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @dead() {
    entry:
      %unused = add i32 9, 9
      ret i32 %unused
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  auto *Dead = Module->getFunction("dead");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);
  ASSERT_NE(Dead, nullptr);

  auto Result = elimination::runInterElimConstantPropagation(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *DeadInst = findInstructionByName(Dead, "unused");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(DeadInst, nullptr);
  auto *CallerFacts = Result.tryIN(MainRet, {});
  ASSERT_NE(CallerFacts, nullptr);
  auto It = CallerFacts->find(Call);
  ASSERT_NE(It, CallerFacts->end());
  if (It->second.isConstant()) {
    auto *CI = llvm::dyn_cast<llvm::ConstantInt>(It->second.getConstant());
    ASSERT_NE(CI, nullptr);
    EXPECT_EQ(CI->getSExtValue(), 3);
  }

  auto *CalleeFacts = Result.tryIN(&*Id->getEntryBlock().begin(), {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  auto FormalIt = CalleeFacts->find(&*Id->arg_begin());
  ASSERT_NE(FormalIt, CalleeFacts->end());
  if (FormalIt->second.isConstant()) {
    auto *FormalCI =
        llvm::dyn_cast<llvm::ConstantInt>(FormalIt->second.getConstant());
    ASSERT_NE(FormalCI, nullptr);
    EXPECT_EQ(FormalCI->getSExtValue(), 3);
  }

  EXPECT_FALSE(It->second.isUnknown());
  EXPECT_FALSE(FormalIt->second.isUnknown());

  EXPECT_EQ(Result.tryIN(DeadInst, {}), nullptr);
}
TEST_F(APATest, ForwardSummaryConstantPropagationMatchesScalarReturnWorklist) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);

  auto Worklist = elimination::runInterElimConstantPropagation(Main);
  auto Summary = elimination::runInterSummaryElimConstantPropagation(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeEntry = &*Id->getEntryBlock().begin();
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);

  auto *WorklistRetFacts = Worklist.tryIN(MainRet, {});
  auto *SummaryRetFacts = Summary.tryIN(MainRet, {});
  ASSERT_NE(WorklistRetFacts, nullptr);
  ASSERT_NE(SummaryRetFacts, nullptr);
  auto WorklistCallIt = WorklistRetFacts->find(Call);
  auto SummaryCallIt = SummaryRetFacts->find(Call);
  ASSERT_NE(WorklistCallIt, WorklistRetFacts->end());
  ASSERT_NE(SummaryCallIt, SummaryRetFacts->end());
  EXPECT_EQ(WorklistCallIt->second.isConstant(),
            SummaryCallIt->second.isConstant());
  if (SummaryCallIt->second.isConstant()) {
    auto *CI =
        llvm::dyn_cast<llvm::ConstantInt>(SummaryCallIt->second.getConstant());
    ASSERT_NE(CI, nullptr);
    EXPECT_EQ(CI->getSExtValue(), 3);
  }

  auto *WorklistCalleeFacts = Worklist.tryIN(CalleeEntry, {Call});
  auto *SummaryCalleeFacts = Summary.tryIN(CalleeEntry, {Call});
  ASSERT_NE(WorklistCalleeFacts, nullptr);
  ASSERT_NE(SummaryCalleeFacts, nullptr);
  EXPECT_EQ(WorklistCalleeFacts->count(&*Id->arg_begin()),
            SummaryCalleeFacts->count(&*Id->arg_begin()));
}
TEST_F(APATest, ForwardSummaryConstantPropagationMatchesNestedCallWorklist) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @wrap(i32 %y) {
    entry:
      %inner = call i32 @id(i32 %y)
      ret i32 %inner
    }

    define i32 @main() {
    entry:
      %seed = add i32 4, 5
      %call = call i32 @wrap(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto Worklist = elimination::runInterElimConstantPropagation(Main);
  auto Summary = elimination::runInterSummaryElimConstantPropagation(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);

  auto *MainCall = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  ASSERT_NE(MainCall, nullptr);
  ASSERT_NE(MainRet, nullptr);

  auto *WorklistFacts = Worklist.tryIN(MainRet, {});
  auto *SummaryFacts = Summary.tryIN(MainRet, {});
  ASSERT_NE(WorklistFacts, nullptr);
  ASSERT_NE(SummaryFacts, nullptr);

  auto WorklistIt = WorklistFacts->find(MainCall);
  auto SummaryIt = SummaryFacts->find(MainCall);
  ASSERT_NE(WorklistIt, WorklistFacts->end());
  ASSERT_NE(SummaryIt, SummaryFacts->end());
  EXPECT_EQ(WorklistIt->second.isConstant(), SummaryIt->second.isConstant());
  if (SummaryIt->second.isConstant()) {
    auto *CI =
        llvm::dyn_cast<llvm::ConstantInt>(SummaryIt->second.getConstant());
    ASSERT_NE(CI, nullptr);
    EXPECT_EQ(CI->getSExtValue(), 9);
  }
}
TEST_F(APATest, InterproceduralLiveVariablesBackwardAcrossCall) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %unused = add i32 %seed, 5
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);

  auto Result = elimination::runInterElimLiveVariables(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Seed = findInstructionByName(Main, "seed");
  auto *Unused = findInstructionByName(Main, "unused");
  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeRet = findFirst<llvm::ReturnInst>(Id);
  ASSERT_NE(Seed, nullptr);
  ASSERT_NE(Unused, nullptr);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeRet, nullptr);

  auto *CallFacts = Result.tryIN(Call, {});
  ASSERT_NE(CallFacts, nullptr);
  EXPECT_NE(CallFacts->find(Seed), CallFacts->end());
  EXPECT_EQ(CallFacts->find(Unused), CallFacts->end());

  auto *CalleeFacts = Result.tryOUT(CalleeRet, {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  EXPECT_NE(CalleeFacts->find(&*Id->arg_begin()), CalleeFacts->end());

  auto *RetFacts = Result.tryOUT(MainRet, {});
  ASSERT_NE(RetFacts, nullptr);
  EXPECT_NE(RetFacts->find(Call), RetFacts->end());
}
TEST_F(APATest, InterproceduralConstantPropagationThroughPointerArgument) {
  const char *Source = R"(
    define void @set42(i32* %p) {
    entry:
      store i32 42, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i32
      store i32 7, i32* %slot
      call void @set42(i32* %slot)
      %loaded = load i32, i32* %slot
      ret i32 %loaded
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto Result = elimination::runInterElimConstantPropagation(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Slot = findInstructionByName(Main, "slot");
  auto *Load = findInstructionByName(Main, "loaded");
  auto *Ret = findFirst<llvm::ReturnInst>(Main);
  ASSERT_NE(Slot, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Ret, nullptr);

  auto *Facts = Result.tryIN(Ret, {});
  ASSERT_NE(Facts, nullptr);
  auto SlotIt = Facts->find(Slot);
  ASSERT_NE(SlotIt, Facts->end());
  EXPECT_FALSE(SlotIt->second.isUnknown());

  auto LoadIt = Facts->find(Load);
  ASSERT_NE(LoadIt, Facts->end());
  EXPECT_FALSE(LoadIt->second.isUnknown());
}
TEST_F(APATest,
       ForwardSummaryConstantPropagationMatchesPointerArgumentWorklist) {
  const char *Source = R"(
    define void @set42(i32* %p) {
    entry:
      store i32 42, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i32
      store i32 7, i32* %slot
      call void @set42(i32* %slot)
      %loaded = load i32, i32* %slot
      ret i32 %loaded
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto Worklist = elimination::runInterElimConstantPropagation(Main);
  auto Summary = elimination::runInterSummaryElimConstantPropagation(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);

  auto *Slot = findInstructionByName(Main, "slot");
  auto *Load = findInstructionByName(Main, "loaded");
  auto *Ret = findFirst<llvm::ReturnInst>(Main);
  ASSERT_NE(Slot, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Ret, nullptr);

  auto *WorklistFacts = Worklist.tryIN(Ret, {});
  auto *SummaryFacts = Summary.tryIN(Ret, {});
  ASSERT_NE(WorklistFacts, nullptr);
  ASSERT_NE(SummaryFacts, nullptr);
  EXPECT_EQ(WorklistFacts->count(Slot), SummaryFacts->count(Slot));
  EXPECT_EQ(WorklistFacts->count(Load), SummaryFacts->count(Load));
  if (auto It = SummaryFacts->find(Load); It != SummaryFacts->end()) {
    EXPECT_FALSE(It->second.isUnknown());
  }
}
TEST_F(APATest, InterproceduralReachingDefinitionsAcrossCall) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);

  auto Result = elimination::runInterElimReachingDefinitions(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeEntry = &*Id->getEntryBlock().begin();
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);

  auto *CalleeFacts = Result.tryIN(CalleeEntry, {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  EXPECT_NE(CalleeFacts->find(&*Id->arg_begin()), CalleeFacts->end());

  auto *RetFacts = Result.tryIN(MainRet, {});
  ASSERT_NE(RetFacts, nullptr);
  EXPECT_NE(RetFacts->find(Call), RetFacts->end());
}
TEST_F(APATest, ForwardSummaryReachingDefinitionsMatchesWorklist) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);

  auto Worklist = elimination::runInterElimReachingDefinitions(Main);
  auto Summary = elimination::runInterSummaryElimReachingDefinitions(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeEntry = &*Id->getEntryBlock().begin();
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);

  auto *WorklistCalleeFacts = Worklist.tryIN(CalleeEntry, {Call});
  auto *SummaryCalleeFacts = Summary.tryIN(CalleeEntry, {Call});
  ASSERT_NE(WorklistCalleeFacts, nullptr);
  ASSERT_NE(SummaryCalleeFacts, nullptr);
  EXPECT_EQ(WorklistCalleeFacts->count(&*Id->arg_begin()),
            SummaryCalleeFacts->count(&*Id->arg_begin()));

  auto *WorklistRetFacts = Worklist.tryIN(MainRet, {});
  auto *SummaryRetFacts = Summary.tryIN(MainRet, {});
  ASSERT_NE(WorklistRetFacts, nullptr);
  ASSERT_NE(SummaryRetFacts, nullptr);
  EXPECT_EQ(WorklistRetFacts->count(Call), SummaryRetFacts->count(Call));
}
TEST_F(APATest, InterproceduralUninitializedVariablesAcrossCall) {
  const char *Source = R"(
    define i32 @loadit(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %slot = alloca i32
      %call = call i32 @loadit(i32* %slot)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *LoadIt = Module->getFunction("loadit");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(LoadIt, nullptr);

  auto Result = elimination::runInterElimUninitVariables(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *CalleeEntry = &*LoadIt->getEntryBlock().begin();
  auto *Load = findInstructionByName(LoadIt, "v");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);
  ASSERT_NE(Load, nullptr);

  auto *CalleeFacts = Result.tryIN(CalleeEntry, {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  EXPECT_NE(CalleeFacts->find(&*LoadIt->arg_begin()), CalleeFacts->end());

  auto *LoadFacts = Result.tryOUT(Load, {Call});
  ASSERT_NE(LoadFacts, nullptr);
  EXPECT_NE(LoadFacts->find(Load), LoadFacts->end());
}
TEST_F(APATest, ForwardSummaryUninitializedVariablesMatchesWorklist) {
  const char *Source = R"(
    define i32 @loadit(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %slot = alloca i32
      %call = call i32 @loadit(i32* %slot)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *LoadIt = Module->getFunction("loadit");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(LoadIt, nullptr);

  auto Worklist = elimination::runInterElimUninitVariables(Main);
  auto Summary = elimination::runInterSummaryElimUninitVariables(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *CalleeEntry = &*LoadIt->getEntryBlock().begin();
  auto *Load = findInstructionByName(LoadIt, "v");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);
  ASSERT_NE(Load, nullptr);

  auto *WorklistCalleeFacts = Worklist.tryIN(CalleeEntry, {Call});
  auto *SummaryCalleeFacts = Summary.tryIN(CalleeEntry, {Call});
  ASSERT_NE(WorklistCalleeFacts, nullptr);
  ASSERT_NE(SummaryCalleeFacts, nullptr);
  EXPECT_EQ(WorklistCalleeFacts->count(&*LoadIt->arg_begin()),
            SummaryCalleeFacts->count(&*LoadIt->arg_begin()));

  auto *WorklistLoadFacts = Worklist.tryOUT(Load, {Call});
  auto *SummaryLoadFacts = Summary.tryOUT(Load, {Call});
  ASSERT_NE(WorklistLoadFacts, nullptr);
  ASSERT_NE(SummaryLoadFacts, nullptr);
  EXPECT_EQ(WorklistLoadFacts->count(Load), SummaryLoadFacts->count(Load));
}
TEST_F(APATest, ForwardSummaryLocksetMatchesWorklist) {
  const char *Source = R"(
    declare void @pthread_mutex_lock(i8*)

    define void @main() {
    entry:
      %slot = alloca i8
      call void @pthread_mutex_lock(i8* %slot)
      ret void
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto Worklist = elimination::runInterElimLockset(Main);
  auto Summary = elimination::runInterSummaryElimLockset(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *Slot = findInstructionByName(Main, "slot");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(Slot, nullptr);

  auto *WorklistRetFacts = Worklist.tryIN(MainRet, {});
  auto *SummaryRetFacts = Summary.tryIN(MainRet, {});
  ASSERT_NE(WorklistRetFacts, nullptr);
  ASSERT_NE(SummaryRetFacts, nullptr);
  EXPECT_EQ(WorklistRetFacts->count(Slot), SummaryRetFacts->count(Slot));
}
TEST_F(APATest, ForwardSummaryLocksetWrapperPropagatesCalleeReturn) {
  const char *Source = R"(
    declare void @pthread_mutex_lock(i8*)

    define void @take(i8* %m) {
    entry:
      call void @pthread_mutex_lock(i8* %m)
      ret void
    }

    define void @main() {
    entry:
      %slot = alloca i8
      call void @take(i8* %slot)
      ret void
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto Summary = elimination::runInterSummaryElimLockset(Main);
  ASSERT_TRUE(Summary.hasSolveMetadata());
  EXPECT_EQ(Summary.solveStatus(), elimination::SolveStatus::Ok);

  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *Slot = findInstructionByName(Main, "slot");
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(Slot, nullptr);

  auto *SummaryRetFacts = Summary.tryIN(MainRet, {});
  ASSERT_NE(SummaryRetFacts, nullptr);
  EXPECT_NE(SummaryRetFacts->find(Slot), SummaryRetFacts->end());
}
