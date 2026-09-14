#include "Dataflow/WPDS/Analyses/ConstantPropagationAnalysis.h"
#include "Dataflow/WPDS/Analyses/LivenessAnalysis.h"
#include "Dataflow/WPDS/Analyses/TaintAnalysis.h"
#include "Dataflow/WPDS/Analyses/UninitializedVariablesAnalysis.h"
#include "Dataflow/WPDS/Backend/Model.h"
#include "Dataflow/WPDS/Backend/PreparedBackend.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "TestUtils/LLVMHelpers.h"

#include <random>

#include <gtest/gtest.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

using namespace llvm;
using namespace wpds;

namespace {

static bool containsFact(const std::set<Value *> &Facts, Value *V) {
  return Facts.count(V) != 0;
}

class WPDSTest : public ::testing::Test {
protected:
  void SetUp() override {
    DataFlowFacts::ClearUniverse();
    Ctx = std::make_unique<LLVMContext>();
  }

  Value *fact(int N) { return ConstantInt::get(Type::getInt32Ty(*Ctx), N); }

  GenKillTransformer *
  makeTransformer(std::initializer_list<Value *> Kill,
                  std::initializer_list<Value *> Gen,
                  std::map<Value *, DataFlowFacts> Flow = {}) {
    DataFlowFacts KillFacts;
    DataFlowFacts GenFacts;
    for (auto *V : Kill) {
      KillFacts.addFact(V);
    }
    for (auto *V : Gen) {
      GenFacts.addFact(V);
    }
    return GenKillTransformer::makeGenKillTransformer(KillFacts, GenFacts,
                                                      Flow);
  }

  std::unique_ptr<Module> parseTestModule(const char *Name,
                                          const char *Source) {
    auto M = lotus::unittest::parseModule(*Ctx, Source, Name);
    EXPECT_NE(M, nullptr);
    return M;
  }

  std::unique_ptr<Module> createLinearModule() {
    return parseTestModule("linear", R"(
      define i32 @main() {
      entry:
        %first = add i32 1, 2
        %second = add i32 %first, 3
        ret i32 0
      }
    )");
  }

  std::unique_ptr<Module> createBranchJoinModule() {
    return parseTestModule("branch_join", R"(
      define i32 @main() {
      entry:
        br i1 true, label %then, label %else

      then:
        %then_fact = add i32 4, 5
        br label %merge

      else:
        br label %merge

      merge:
        ret i32 0
      }
    )");
  }

  std::unique_ptr<Module> createAliasLimitationModule() {
    return parseTestModule("alias_limitation", R"(
      define i32 @main() {
      entry:
        %p = alloca i32
        %q = getelementptr i32, i32* %p, i32 0
        store i32 1, i32* %p
        %load_q = load i32, i32* %q
        ret i32 %load_q
      }
    )");
  }

  std::unique_ptr<Module> createGlobalStoreLoadModule() {
    return parseTestModule("global_store_load", R"(
      @g = global i32 0

      define i32 @main() {
      entry:
        store i32 9, i32* @g
        %load_g = load i32, i32* @g
        ret i32 %load_g
      }
    )");
  }

  std::unique_ptr<Module> createUnknownCallModule() {
    return parseTestModule("unknown_call", R"(
      @g = global i32 0

      declare i32 @ext(i32*)

      define i32 @main() {
      entry:
        %p = alloca i32
        %ext_result = call i32 @ext(i32* %p)
        ret i32 %ext_result
      }
    )");
  }

  std::unique_ptr<Module> createReadOnlyCallModule() {
    return parseTestModule("readonly_call", R"(
      declare i32 @reader(i32*)

      define i32 @main() {
      entry:
        %p = alloca i32
        %reader_result = call i32 @reader(i32* %p)
        %load_p = load i32, i32* %p
        ret i32 %load_p
      }
    )");
  }

  std::unique_ptr<Module> createMultiCalleeResolverModule() {
    return parseTestModule("multi_callee", R"(
      @g1 = global i32 1
      @g2 = global i32 2

      declare i32* @dispatch()

      define internal i32* @left() {
      entry:
        ret i32* @g1
      }

      define internal i32* @right() {
      entry:
        ret i32* @g2
      }

      define i32 @main() {
      entry:
        %dispatch_result = call i32* @dispatch()
        %after_call = ptrtoint i32* %dispatch_result to i32
        ret i32 %after_call
      }
    )");
  }

  std::unique_ptr<Module> createMixedCalleeResolverModule() {
    return parseTestModule("mixed_callee", R"(
      @known_g = global i32 1
      @unknown_g = global i32 2

      declare i32* @dispatch(i32*)

      define internal i32* @known(i32* %arg) {
      entry:
        ret i32* @known_g
      }

      define i32 @main() {
      entry:
        %p = alloca i32
        %dispatch_result = call i32* @dispatch(i32* %p)
        %after_call = ptrtoint i32* %dispatch_result to i32
        ret i32 %after_call
      }
    )");
  }

  std::unique_ptr<Module> createTwoFunctionModule() {
    return parseTestModule("two_functions", R"(
      @seed = global i32 1

      define i32 @main() {
      entry:
        %main_inst = add i32 1, 2
        ret i32 0
      }

      define internal i32 @helper() {
      entry:
        %helper_inst = add i32 3, 4
        ret i32 0
      }
    )");
  }

  std::unique_ptr<Module> createReturnThroughCalleeModule() {
    return parseTestModule("return_through_callee", R"(
      define internal i32 @id(i32 %arg) {
      entry:
        ret i32 %arg
      }

      define i32 @main() {
      entry:
        %seed = add i32 1, 2
        %call_id = call i32 @id(i32 %seed)
        ret i32 %call_id
      }
    )");
  }

  std::unique_ptr<Module> createRecursiveModule() {
    return parseTestModule("recursive", R"(
      @seed = global i32 0

      define i32 @recur(i32 %n) {
      entry:
        %is_zero = icmp eq i32 %n, 0
        br i1 %is_zero, label %base, label %step
      step:
        %dec = sub i32 %n, 1
        %recursive_call = call i32 @recur(i32 %dec)
        ret i32 %recursive_call
      base:
        ret i32 0
      }

      define i32 @main() {
      entry:
        %result = call i32 @recur(i32 2)
        ret i32 %result
      }
    )");
  }

  std::unique_ptr<Module> createUninitializedLoadValueModule() {
    return parseTestModule("uninit_load_value", R"(
      define i32 @main() {
      entry:
        %p = alloca i32
        %loaded = load i32, i32* %p
        %use_loaded = add i32 %loaded, 1
        ret i32 %use_loaded
      }
    )");
  }

  std::unique_ptr<Module> createUnnamedLivenessModule() {
    auto M = parseTestModule("liveness", R"(
      define i32 @main() {
      entry:
        %tmp = add i32 1, 2
        ret i32 %tmp
      }
    )");
    UnnamedDef = findInstInModule(*M, "tmp");
    EXPECT_NE(UnnamedDef, nullptr);
    if (UnnamedDef != nullptr) {
      UnnamedDef->setName("");
    }
    RetInst = cast<ReturnInst>(M->getFunction("main")->back().getTerminator());
    return M;
  }

  std::unique_ptr<Module> createStorePointerUseModule() {
    auto M = parseTestModule("store_pointer_use", R"(
      define i32 @main() {
      entry:
        %p = alloca i32
        store i32 1, i32* %p
        %ptr_use = ptrtoint i32* %p to i64
        %ptr_use32 = trunc i64 %ptr_use to i32
        ret i32 0
      }
    )");
    StoreInstForLiveness =
        cast<StoreInst>(&*std::next(M->getFunction("main")->front().begin()));
    PointerAllocaForLiveness = findInstInModule(*M, "p");
    PtrUseInst = findInstInModule(*M, "ptr_use");
    return M;
  }

  Instruction *findInstInModule(Module &M, StringRef Name) {
    for (auto &F : M) {
      if (F.isDeclaration()) {
        continue;
      }
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (I.getName() == Name) {
            return &I;
          }
        }
      }
    }
    return nullptr;
  }

  std::unique_ptr<LLVMContext> Ctx;
  Instruction *UnnamedDef = nullptr;
  ReturnInst *RetInst = nullptr;
  StoreInst *StoreInstForLiveness = nullptr;
  Value *PointerAllocaForLiveness = nullptr;
  Instruction *PtrUseInst = nullptr;
};

TEST_F(WPDSTest, CombineIsCommutativeAndIdempotentForMayJoin) {
  auto *A = fact(1);
  auto *B = fact(2);
  auto *C = fact(3);

  auto *Left = makeTransformer({A}, {C});
  auto *Right = makeTransformer({B}, {A});

  auto *JoinLR = Left->combine(Right);
  auto *JoinRL = Right->combine(Left);

  EXPECT_TRUE(JoinLR->equal(JoinRL));
  EXPECT_TRUE(Left->combine(Left)->equal(Left));

  DataFlowFacts Input;
  Input.addFact(A);
  Input.addFact(B);
  DataFlowFacts Output = JoinLR->apply(Input);
  EXPECT_TRUE(Output.containsFact(A));
  EXPECT_TRUE(Output.containsFact(C));
}

TEST_F(WPDSTest, ExtendIsAssociativeAndHasIdentityAndZeroLaws) {
  auto *A = fact(1);
  auto *B = fact(2);
  auto *C = fact(3);
  auto *D = fact(4);

  std::map<Value *, DataFlowFacts> Flow1;
  DataFlowFacts ToB;
  ToB.addFact(B);
  Flow1[A] = ToB;

  std::map<Value *, DataFlowFacts> Flow2;
  DataFlowFacts ToC;
  ToC.addFact(C);
  Flow2[B] = ToC;

  auto *T1 = makeTransformer({}, {}, Flow1);
  auto *T2 = makeTransformer({}, {D}, Flow2);
  auto *T3 = makeTransformer({C}, {A});

  auto *Lhs = T1->extend(T2)->extend(T3);
  auto *Rhs = T1->extend(T2->extend(T3));
  EXPECT_TRUE(Lhs->equal(Rhs));

  EXPECT_TRUE(T1->extend(GenKillTransformer::one())->equal(T1));
  EXPECT_TRUE(GenKillTransformer::one()->extend(T1)->equal(T1));
  EXPECT_TRUE(T1->extend(GenKillTransformer::zero())
                  ->equal(GenKillTransformer::zero()));
  EXPECT_TRUE(GenKillTransformer::zero()->extend(T1)->equal(
      GenKillTransformer::zero()));
  EXPECT_TRUE(T1->combine(GenKillTransformer::zero())->equal(T1));
}

TEST_F(WPDSTest, GenKillValueMatchesItsFiniteDenotationExhaustively) {
  auto *A = fact(61);
  auto *B = fact(62);
  std::vector<Value *> Universe = {A, B};
  std::vector<DataFlowFacts> Inputs;
  std::vector<GenKillValue> Transformers;

  for (unsigned Mask = 0; Mask < 4; ++Mask) {
    std::set<Value *> Facts;
    for (unsigned Bit = 0; Bit < Universe.size(); ++Bit) {
      if ((Mask & (1U << Bit)) != 0) {
        Facts.insert(Universe[Bit]);
      }
    }
    Inputs.emplace_back(Facts);
  }
  for (unsigned KillMask = 0; KillMask < 4; ++KillMask) {
    for (unsigned GenMask = 0; GenMask < 4; ++GenMask) {
      std::set<Value *> Kill;
      std::set<Value *> Gen;
      for (unsigned Bit = 0; Bit < Universe.size(); ++Bit) {
        if ((KillMask & (1U << Bit)) != 0) {
          Kill.insert(Universe[Bit]);
        }
        if ((GenMask & (1U << Bit)) != 0) {
          Gen.insert(Universe[Bit]);
        }
      }
      Transformers.push_back(
          GenKillValue::normalized(DataFlowFacts(Kill), DataFlowFacts(Gen)));
    }
  }

  auto SameDenotation = [&](const GenKillValue &Left,
                            const GenKillValue &Right) {
    for (const DataFlowFacts &Input : Inputs) {
      if (!DataFlowFacts::Eq(Left.apply(Input), Right.apply(Input))) {
        return false;
      }
    }
    return true;
  };

  for (const GenKillValue &Left : Transformers) {
    EXPECT_TRUE(SameDenotation(Left.extend(GenKillValue::one()), Left));
    EXPECT_TRUE(SameDenotation(GenKillValue::one().extend(Left), Left));
    EXPECT_TRUE(Left.extend(GenKillValue::zero()).equal(GenKillValue::zero()));
    EXPECT_TRUE(GenKillValue::zero().extend(Left).equal(GenKillValue::zero()));
    EXPECT_TRUE(SameDenotation(Left.combine(GenKillValue::zero()), Left));

    for (const GenKillValue &Right : Transformers) {
      for (const DataFlowFacts &Input : Inputs) {
        EXPECT_TRUE(DataFlowFacts::Eq(Left.extend(Right).apply(Input),
                                      Right.apply(Left.apply(Input))));
        EXPECT_TRUE(DataFlowFacts::Eq(
            Left.combine(Right).apply(Input),
            DataFlowFacts::Union(Left.apply(Input), Right.apply(Input))));
      }
      for (const GenKillValue &Third : Transformers) {
        EXPECT_TRUE(SameDenotation(Left.extend(Right).extend(Third),
                                   Left.extend(Right.extend(Third))));
        EXPECT_TRUE(
            SameDenotation(Left.extend(Right.combine(Third)),
                           Left.extend(Right).combine(Left.extend(Third))));
        EXPECT_TRUE(
            SameDenotation(Left.combine(Right).extend(Third),
                           Left.extend(Third).combine(Right.extend(Third))));
      }
    }
  }
}

TEST_F(WPDSTest, NoPathZeroIsDistinctFromReachableKillAll) {
  auto *A = fact(63);
  auto *B = fact(64);
  GenKillValue KillAll(DataFlowFacts::UniverseSet(), DataFlowFacts::EmptySet());
  GenKillValue GenerateB = GenKillValue::normalized(
      DataFlowFacts::EmptySet(), DataFlowFacts(std::set<Value *>{B}));

  EXPECT_FALSE(KillAll.equal(GenKillValue::zero()));
  EXPECT_TRUE(KillAll.apply(DataFlowFacts(std::set<Value *>{A})).isEmpty());
  EXPECT_TRUE(KillAll.extend(GenerateB)
                  .apply(DataFlowFacts(std::set<Value *>{A}))
                  .containsFact(B));
  EXPECT_TRUE(
      GenKillValue::zero().extend(GenerateB).equal(GenKillValue::zero()));
}

TEST_F(WPDSTest, RelationalGenKillValuesSatisfySemiringLaws) {
  auto *A = fact(65);
  auto *B = fact(66);
  auto *C = fact(67);
  std::vector<Value *> Universe = {A, B, C};
  std::vector<DataFlowFacts> Inputs;
  for (unsigned Mask = 0; Mask < 8; ++Mask) {
    std::set<Value *> Facts;
    for (unsigned Bit = 0; Bit < Universe.size(); ++Bit) {
      if ((Mask & (1U << Bit)) != 0) {
        Facts.insert(Universe[Bit]);
      }
    }
    Inputs.emplace_back(Facts);
  }

  auto FlowValue = [](Value *From, std::initializer_list<Value *> To) {
    std::map<Value *, DataFlowFacts> Flow;
    Flow[From] = DataFlowFacts(std::set<Value *>(To.begin(), To.end()));
    return GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                    DataFlowFacts::EmptySet(), Flow);
  };
  std::vector<GenKillValue> Values = {
      GenKillValue::one(),
      FlowValue(A, {B}),
      FlowValue(B, {C}),
      FlowValue(C, {A, B}),
      GenKillValue::normalized(DataFlowFacts(std::set<Value *>{A}),
                               DataFlowFacts::EmptySet()),
      GenKillValue::normalized(DataFlowFacts::EmptySet(),
                               DataFlowFacts(std::set<Value *>{C}))};

  auto Same = [&](const GenKillValue &Left, const GenKillValue &Right) {
    for (const DataFlowFacts &Input : Inputs) {
      if (!DataFlowFacts::Eq(Left.apply(Input), Right.apply(Input))) {
        return false;
      }
    }
    return true;
  };
  for (const GenKillValue &X : Values) {
    for (const GenKillValue &Y : Values) {
      for (const GenKillValue &Z : Values) {
        EXPECT_TRUE(Same(X.extend(Y).extend(Z), X.extend(Y.extend(Z))));
        EXPECT_TRUE(
            Same(X.extend(Y.combine(Z)), X.extend(Y).combine(X.extend(Z))));
        EXPECT_TRUE(
            Same(X.combine(Y).extend(Z), X.extend(Z).combine(Y.extend(Z))));
      }
    }
  }
}

TEST_F(WPDSTest, UniverseSetSupportsSubtractionAndRemoval) {
  auto *A = fact(1);
  auto *B = fact(2);
  auto *C = fact(3);

  DataFlowFacts universe = DataFlowFacts::UniverseSet();
  universe.removeFact(A);
  EXPECT_FALSE(universe.containsFact(A));
  EXPECT_TRUE(universe.containsFact(B));

  DataFlowFacts finite;
  finite.addFact(B);
  DataFlowFacts diff = DataFlowFacts::Diff(universe, finite);
  EXPECT_FALSE(diff.containsFact(A));
  EXPECT_FALSE(diff.containsFact(B));
  EXPECT_TRUE(diff.containsFact(C));
}

TEST_F(WPDSTest, ForwardAnalysisRetainsResultForAccessorQueries) {
  auto M = createLinearModule();
  InterProceduralDataFlowEngine Engine;
  auto *SeedFact = fact(7);

  auto Result = Engine.runForwardAnalysis(
      *M, [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "first") {
          return makeTransformer({}, {SeedFact});
        }
        return GenKillTransformer::one();
      });

  auto *First = findInstInModule(*M, "first");
  auto *Second = findInstInModule(*M, "second");
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);

  EXPECT_EQ(Result->OUT(First), Engine.getOutSet(First));
  EXPECT_EQ(Result->IN(Second), Engine.getInSet(Second));
  EXPECT_TRUE(containsFact(Result->OUT(First), SeedFact));
}

TEST_F(WPDSTest, BackendSelectionIsExplicitAndLegacyRemainsDefault) {
  EXPECT_EQ(parseWPDSBackend("legacy"), WPDSBackendKind::Legacy);
  EXPECT_EQ(parseWPDSBackend("wali-fwpds"), WPDSBackendKind::WaliFWPDS);
  EXPECT_EQ(parseWPDSBackend("wali-swpds"), WPDSBackendKind::WaliSWPDS);
  EXPECT_FALSE(parseWPDSBackend("auto").has_value());

  InterProceduralDataFlowEngine DefaultEngine;
  EXPECT_EQ(DefaultEngine.getBackendOptions().backend, WPDSBackendKind::Legacy);

#ifndef LOTUS_ENABLE_WALI_OPENNWA
  EXPECT_FALSE(isWPDSBackendAvailable(WPDSBackendKind::WaliFWPDS));
  auto M = createLinearModule();
  InterProceduralDataFlowEngine DisabledEngine(
      {WPDSBackendKind::WaliFWPDS, false, false});
  auto Result = DisabledEngine.runForwardAnalysis(
      *M, [](Instruction *) { return GenKillTransformer::one(); });
  EXPECT_EQ(Result, nullptr);
  EXPECT_NE(DisabledEngine.getLastError().find("LOTUS_ENABLE_WALI_OPENNWA"),
            std::string::npos);
#else
  EXPECT_TRUE(isWPDSBackendAvailable(WPDSBackendKind::WaliFWPDS));
  EXPECT_TRUE(isWPDSBackendAvailable(WPDSBackendKind::WaliSWPDS));
#endif
}

#ifdef LOTUS_ENABLE_WALI_OPENNWA
TEST_F(WPDSTest, WaliBackendsMatchLegacyOnForwardAndBackwardGenKill) {
  for (WPDSBackendKind Backend :
       {WPDSBackendKind::WaliFWPDS, WPDSBackendKind::WaliSWPDS}) {
    auto M = createLinearModule();
    auto *Generated = fact(31);
    auto *Killed = fact(32);
    auto Transfer = [&](Instruction *I) -> GenKillTransformer * {
      if (I->getName() == "first") {
        return makeTransformer({Killed}, {Generated});
      }
      if (I->getName() == "second") {
        return makeTransformer({Generated}, {Killed});
      }
      return GenKillTransformer::one();
    };

    InterProceduralDataFlowEngine Legacy;
    auto Expected = Legacy.runForwardAnalysis(*M, Transfer, {Killed});
    ASSERT_NE(Expected, nullptr);

    InterProceduralDataFlowEngine Selected({Backend, false, true});
    auto Actual = Selected.runForwardAnalysis(*M, Transfer, {Killed});
    ASSERT_NE(Actual, nullptr) << Selected.getLastError();
    for (Function &F : *M) {
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          EXPECT_EQ(Actual->IN(&I), Expected->IN(&I));
          EXPECT_EQ(Actual->OUT(&I), Expected->OUT(&I));
        }
      }
    }
    EXPECT_EQ(Selected.getLastBackendStatistics().effectiveBackend, Backend);
    EXPECT_EQ(Selected.getLastBackendStatistics().preparationCount, 1u);
    EXPECT_EQ(Selected.getLastBackendStatistics().queryCount, 1u);

    InterProceduralDataFlowEngine LegacyBackward;
    Expected = LegacyBackward.runBackwardAnalysis(*M, Transfer, {Killed});
    ASSERT_NE(Expected, nullptr);
    InterProceduralDataFlowEngine SelectedBackward({Backend, false, true});
    Actual = SelectedBackward.runBackwardAnalysis(*M, Transfer, {Killed});
    ASSERT_NE(Actual, nullptr) << SelectedBackward.getLastError();
    for (Function &F : *M) {
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          EXPECT_EQ(Actual->IN(&I), Expected->IN(&I));
          EXPECT_EQ(Actual->OUT(&I), Expected->OUT(&I));
        }
      }
    }
  }
}

TEST_F(WPDSTest, WaliSelectionRejectsLegacyAutomatonCallbacksExplicitly) {
  auto M = createLinearModule();
  InterProceduralDataFlowEngine Engine(
      {WPDSBackendKind::WaliFWPDS, false, false});
  bool BuilderCalled = false;
  auto Result = Engine.runForwardAnalysisWithAutomaton(
      *M, [](Instruction *) { return GenKillTransformer::one(); },
      [&](CA<GenKillTransformer> &) { BuilderCalled = true; });
  EXPECT_EQ(Result, nullptr);
  EXPECT_FALSE(BuilderCalled);
  EXPECT_NE(Engine.getLastError().find("only by the legacy"),
            std::string::npos);
}

TEST_F(WPDSTest, PreparedSessionsReuseOneModelForDistinctQueries) {
  for (WPDSBackendKind Backend :
       {WPDSBackendKind::Legacy, WPDSBackendKind::WaliFWPDS,
        WPDSBackendKind::WaliSWPDS}) {
    auto M = createLinearModule();
    auto *A = fact(41);
    auto *B = fact(42);
    std::size_t instructionCount = 0;
    for (Function &F : *M) {
      for (BasicBlock &BB : F) {
        instructionCount += BB.size();
      }
    }

    std::size_t callbackCount = 0;
    auto Transfer = [&](Instruction *I) -> GenKillTransformer * {
      ++callbackCount;
      if (I->getName() == "first") {
        return makeTransformer({A}, {B});
      }
      if (I->getName() == "second") {
        return makeTransformer({B}, {A});
      }
      return GenKillTransformer::one();
    };

    InterProceduralDataFlowEngine Engine(
        {Backend, Backend != WPDSBackendKind::Legacy, true});
    auto Prepared = Engine.prepareForwardAnalysis(*M, Transfer);
    ASSERT_NE(Prepared, nullptr) << Engine.getLastError();
    EXPECT_EQ(callbackCount, instructionCount);

    auto ResultA = Prepared->solve({A});
    ASSERT_NE(ResultA, nullptr) << Prepared->getLastError();
    auto ResultB = Prepared->solve({B});
    ASSERT_NE(ResultB, nullptr) << Prepared->getLastError();
    auto ResultAAgain = Prepared->solve({A});
    ASSERT_NE(ResultAAgain, nullptr) << Prepared->getLastError();
    EXPECT_EQ(callbackCount, instructionCount);

    for (Function &F : *M) {
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          EXPECT_EQ(ResultA->IN(&I), ResultAAgain->IN(&I));
          EXPECT_EQ(ResultA->OUT(&I), ResultAAgain->OUT(&I));
        }
      }
    }
    EXPECT_EQ(Prepared->getStatistics().preparationCount, 1u);
    EXPECT_EQ(Prepared->getStatistics().queryCount, 3u);
    EXPECT_EQ(Prepared->getStatistics().effectiveBackend, Backend);
    Prepared.reset();
    auto *First = findInstInModule(*M, "first");
    ASSERT_NE(First, nullptr);
    EXPECT_EQ(ResultA->OUT(First), ResultAAgain->OUT(First));
  }
}

TEST_F(WPDSTest, WaliPreparedSessionsRemainIsolatedWhenInterleaved) {
  for (WPDSBackendKind Backend :
       {WPDSBackendKind::WaliFWPDS, WPDSBackendKind::WaliSWPDS}) {
    auto FirstModule = createLinearModule();
    auto SecondModule = createBranchJoinModule();
    auto *FirstSeed = fact(43);
    auto *SecondSeed = fact(44);
    InterProceduralDataFlowEngine FirstEngine({Backend, true, true});
    InterProceduralDataFlowEngine SecondEngine({Backend, true, true});
    auto Identity = [](Instruction *) { return GenKillTransformer::one(); };
    auto FirstSession =
        FirstEngine.prepareForwardAnalysis(*FirstModule, Identity);
    auto SecondSession =
        SecondEngine.prepareForwardAnalysis(*SecondModule, Identity);
    ASSERT_NE(FirstSession, nullptr) << FirstEngine.getLastError();
    ASSERT_NE(SecondSession, nullptr) << SecondEngine.getLastError();

    auto FirstA = FirstSession->solve({FirstSeed});
    auto Second = SecondSession->solve({SecondSeed});
    auto FirstB = FirstSession->solve({FirstSeed});
    ASSERT_NE(FirstA, nullptr) << FirstSession->getLastError();
    ASSERT_NE(Second, nullptr) << SecondSession->getLastError();
    ASSERT_NE(FirstB, nullptr) << FirstSession->getLastError();

    auto *FirstInstruction = findInstInModule(*FirstModule, "first");
    auto *JoinInstruction =
        SecondModule->getFunction("main")->back().getTerminator();
    ASSERT_NE(FirstInstruction, nullptr);
    ASSERT_NE(JoinInstruction, nullptr);
    EXPECT_EQ(FirstA->OUT(FirstInstruction), FirstB->OUT(FirstInstruction));
    EXPECT_TRUE(containsFact(FirstB->OUT(FirstInstruction), FirstSeed));
    EXPECT_TRUE(containsFact(Second->IN(JoinInstruction), SecondSeed));
    EXPECT_FALSE(containsFact(Second->IN(JoinInstruction), FirstSeed));
    EXPECT_EQ(FirstSession->getStatistics().preparationCount, 1u);
    EXPECT_EQ(SecondSession->getStatistics().preparationCount, 1u);
  }
}

TEST_F(WPDSTest, SyntheticPushPopModelSupportsPrestarAndPoststar) {
  auto *A = fact(51);
  auto *B = fact(52);
  backend::Model Model;
  auto Q = Model.addControlState("q");
  auto Start = Model.addStackSymbol("start");
  auto Call = Model.addStackSymbol("call");
  auto CalleeEntry = Model.addStackSymbol("callee-entry");
  auto CalleeExit = Model.addStackSymbol("callee-exit");
  auto Continuation = Model.addStackSymbol("continuation");
  auto Done = Model.addStackSymbol("done");
  Model.addProcedureEntry(Start);
  Model.addProcedureEntry(CalleeEntry);

  GenKillValue GenerateA = GenKillValue::normalized(
      DataFlowFacts::EmptySet(), DataFlowFacts(std::set<Value *>{A}));
  GenKillValue KillAThenGenerateB = GenKillValue::normalized(
      DataFlowFacts(std::set<Value *>{A}), DataFlowFacts(std::set<Value *>{B}));
  Model.addReplaceRule(Q, Start, Q, Call, GenerateA, "generate a");
  Model.addPushRule(Q, Call, Q, CalleeEntry, Continuation, GenKillValue::one(),
                    "call");
  Model.addReplaceRule(Q, CalleeEntry, Q, CalleeExit, KillAThenGenerateB,
                       "callee body");
  Model.addPopRule(Q, CalleeExit, Q, GenKillValue::one(), "return");
  Model.addReplaceRule(Q, Continuation, Q, Done, GenKillValue::one(),
                       "continuation");

  for (WPDSBackendKind Backend :
       {WPDSBackendKind::Legacy, WPDSBackendKind::WaliFWPDS,
        WPDSBackendKind::WaliSWPDS}) {
    for (WPDSQueryKind Operation :
         {WPDSQueryKind::PostStar, WPDSQueryKind::PreStar}) {
      SCOPED_TRACE(::testing::Message() << "operation=" << toString(Operation));
      std::string Error;
      WPDSBackendOptions Options{Backend, false, true};
      auto Prepared = backend::prepareBackend(Model, Options, Operation,
                                              {Start, CalleeEntry}, Error);
      ASSERT_NE(Prepared, nullptr) << Error;

      backend::Query Query;
      Query.operation = Operation;
      Query.initialState = Q;
      Query.roots = {Operation == WPDSQueryKind::PostStar ? Start : Done};
      Query.seed = GenKillValue::one();
      backend::QueryResult Result;
      backend::Query Invalid = Query;
      Invalid.roots = {999999};
      EXPECT_FALSE(Prepared->solve(Invalid, Result, Error));
      EXPECT_FALSE(Error.empty());
      Error.clear();
      ASSERT_TRUE(Prepared->solve(Query, Result, Error)) << Error;

      auto Observed = Result.observations.find(
          Operation == WPDSQueryKind::PostStar ? Done : Start);
      ASSERT_NE(Observed, Result.observations.end());
      ASSERT_TRUE(Observed->second.reachable);
      DataFlowFacts Facts =
          Observed->second.summary.apply(DataFlowFacts::EmptySet());
      EXPECT_FALSE(Facts.containsFact(A));
      EXPECT_TRUE(Facts.containsFact(B));

      backend::Query Alternate = Query;
      Alternate.roots = {Operation == WPDSQueryKind::PostStar ? CalleeEntry
                                                              : CalleeExit};
      backend::QueryResult AlternateResult;
      ASSERT_TRUE(Prepared->solve(Alternate, AlternateResult, Error)) << Error;
      backend::QueryResult Repeated;
      ASSERT_TRUE(Prepared->solve(Query, Repeated, Error)) << Error;
      for (backend::StackSymbolId Symbol = 1;
           Symbol < Model.stackSymbolNames().size(); ++Symbol) {
        EXPECT_EQ(Result.observations.at(Symbol).reachable,
                  Repeated.observations.at(Symbol).reachable);
        if (Result.observations.at(Symbol).reachable) {
          EXPECT_TRUE(Result.observations.at(Symbol).summary.semanticallyEqual(
              Repeated.observations.at(Symbol).summary));
        }
      }
      EXPECT_EQ(Prepared->statistics().preparationCount, 1u);
      EXPECT_EQ(Prepared->statistics().queryCount, 3u);
      EXPECT_EQ(Prepared->statistics().effectiveBackend, Backend);
    }
  }
}

TEST_F(WPDSTest, SyntheticRelationalWeightsComposeInExecutionOrder) {
  auto *A = fact(53);
  auto *B = fact(54);
  auto *C = fact(55);
  backend::Model Model;
  auto Q = Model.addControlState("q");
  auto Start = Model.addStackSymbol("start");
  auto Middle = Model.addStackSymbol("middle");
  auto Done = Model.addStackSymbol("done");
  Model.addProcedureEntry(Start);

  std::map<Value *, DataFlowFacts> FirstFlow;
  FirstFlow[A] = DataFlowFacts(std::set<Value *>{B});
  std::map<Value *, DataFlowFacts> SecondFlow;
  SecondFlow[B] = DataFlowFacts(std::set<Value *>{C});
  Model.addReplaceRule(Q, Start, Q, Middle,
                       GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                                DataFlowFacts::EmptySet(),
                                                FirstFlow),
                       "a to b");
  Model.addReplaceRule(Q, Middle, Q, Done,
                       GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                                DataFlowFacts::EmptySet(),
                                                SecondFlow),
                       "b to c");

  for (WPDSBackendKind Backend :
       {WPDSBackendKind::Legacy, WPDSBackendKind::WaliFWPDS,
        WPDSBackendKind::WaliSWPDS}) {
    std::string Error;
    auto Prepared = backend::prepareBackend(
        Model, {Backend, false, true}, WPDSQueryKind::PostStar, {Start}, Error);
    ASSERT_NE(Prepared, nullptr) << Error;
    backend::Query Query;
    Query.operation = WPDSQueryKind::PostStar;
    Query.initialState = Q;
    Query.roots = {Start};
    Query.seed = GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                          DataFlowFacts(std::set<Value *>{A}));
    backend::QueryResult Result;
    ASSERT_TRUE(Prepared->solve(Query, Result, Error)) << Error;
    const auto &Observation = Result.observations.at(Done);
    ASSERT_TRUE(Observation.reachable);
    DataFlowFacts Facts = Observation.summary.apply(DataFlowFacts::EmptySet());
    EXPECT_TRUE(Facts.containsFact(A));
    EXPECT_TRUE(Facts.containsFact(B));
    EXPECT_TRUE(Facts.containsFact(C));
  }
}

TEST_F(WPDSTest, CallAndReturnWeightsComposeInExecutionOrder) {
  auto *A = fact(59);
  GenKillValue GenerateA = GenKillValue::normalized(
      DataFlowFacts::EmptySet(), DataFlowFacts(std::set<Value *>{A}));
  GenKillValue KillA = GenKillValue::normalized(
      DataFlowFacts(std::set<Value *>{A}), DataFlowFacts::EmptySet());
  struct WeightCase {
    GenKillValue Call;
    GenKillValue Body;
    GenKillValue Return;
    bool ExpectedA;
  };
  std::vector<WeightCase> Cases = {
      {GenerateA, KillA, GenKillValue::one(), false},
      {GenKillValue::one(), GenerateA, KillA, false},
      {KillA, GenKillValue::one(), GenerateA, true},
  };

  for (std::size_t CaseIndex = 0; CaseIndex < Cases.size(); ++CaseIndex) {
    SCOPED_TRACE(::testing::Message() << "case=" << CaseIndex);
    const WeightCase &Weights = Cases[CaseIndex];
    backend::Model Model;
    auto Q = Model.addControlState("q");
    auto Start = Model.addStackSymbol("start");
    auto Entry = Model.addStackSymbol("entry");
    auto Exit = Model.addStackSymbol("exit");
    auto Continuation = Model.addStackSymbol("continuation");
    Model.addProcedureEntry(Start);
    Model.addProcedureEntry(Entry);
    Model.addPushRule(Q, Start, Q, Entry, Continuation, Weights.Call, "call");
    Model.addReplaceRule(Q, Entry, Q, Exit, Weights.Body, "body");
    Model.addPopRule(Q, Exit, Q, Weights.Return, "return");

    for (WPDSBackendKind Backend :
         {WPDSBackendKind::Legacy, WPDSBackendKind::WaliFWPDS,
          WPDSBackendKind::WaliSWPDS}) {
      SCOPED_TRACE(::testing::Message() << "backend=" << toString(Backend));
      std::string Error;
      auto Prepared = backend::prepareBackend(Model, {Backend, false, false},
                                              WPDSQueryKind::PostStar,
                                              {Start, Entry}, Error);
      ASSERT_NE(Prepared, nullptr) << Error;
      backend::Query Query;
      Query.operation = WPDSQueryKind::PostStar;
      Query.initialState = Q;
      Query.roots = {Start};
      Query.seed = GenKillValue::one();
      backend::QueryResult Result;
      ASSERT_TRUE(Prepared->solve(Query, Result, Error)) << Error;
      const auto &Observed = Result.observations.at(Continuation);
      ASSERT_TRUE(Observed.reachable);
      EXPECT_EQ(
          Observed.summary.apply(DataFlowFacts::EmptySet()).containsFact(A),
          Weights.ExpectedA);
    }
  }
}

TEST_F(WPDSTest, DeterministicRandomModelsAgreeAcrossBackends) {
  auto *A = fact(56);
  auto *B = fact(57);
  auto *C = fact(58);
  std::vector<Value *> Facts = {A, B, C};
  constexpr unsigned RandomSeed = 0x4c4f5455U;
  std::mt19937 Random(RandomSeed);

  auto RandomWeight = [&]() {
    std::set<Value *> Kill;
    std::set<Value *> Gen;
    std::map<Value *, DataFlowFacts> Flow;
    for (Value *Fact : Facts) {
      if ((Random() & 3U) == 0) {
        Kill.insert(Fact);
      }
      if ((Random() & 3U) == 0) {
        Gen.insert(Fact);
      }
      std::set<Value *> Targets;
      for (Value *Target : Facts) {
        if ((Random() & 7U) == 0) {
          Targets.insert(Target);
        }
      }
      if (!Targets.empty()) {
        Flow[Fact] = DataFlowFacts(Targets);
      }
    }
    return GenKillValue::normalized(DataFlowFacts(Kill), DataFlowFacts(Gen),
                                    Flow);
  };

  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(::testing::Message()
                 << "random_seed=" << RandomSeed << " case=" << Case);
    backend::Model Model;
    auto Q = Model.addControlState("q");
    auto Start = Model.addStackSymbol("start");
    auto Call = Model.addStackSymbol("call");
    auto Entry = Model.addStackSymbol("entry");
    auto Body = Model.addStackSymbol("body");
    auto Exit = Model.addStackSymbol("exit");
    auto Continuation = Model.addStackSymbol("continuation");
    auto Done = Model.addStackSymbol("done");
    Model.addProcedureEntry(Start);
    Model.addProcedureEntry(Entry);
    Model.addReplaceRule(Q, Start, Q, Call, RandomWeight(), "start");
    Model.addPushRule(Q, Call, Q, Entry, Continuation, RandomWeight(), "call");
    Model.addReplaceRule(Q, Entry, Q, Body, RandomWeight(), "entry");
    Model.addReplaceRule(Q, Body, Q, Exit, RandomWeight(), "body");
    Model.addPopRule(Q, Exit, Q, RandomWeight(), "return");
    Model.addReplaceRule(Q, Continuation, Q, Done, RandomWeight(), "done");
    Model.addReplaceRule(Q, Start, Q, Done, RandomWeight(), "alternate");
    GenKillValue Seed = RandomWeight();
    std::vector<GenKillValue> AlgebraValues = {Seed};
    for (const backend::Rule &Rule : Model.rules()) {
      AlgebraValues.push_back(Rule.weight);
    }
    for (const GenKillValue &X : AlgebraValues) {
      for (const GenKillValue &Y : AlgebraValues) {
        for (const GenKillValue &Z : AlgebraValues) {
          EXPECT_TRUE(
              X.extend(Y).extend(Z).semanticallyEqual(X.extend(Y.extend(Z))));
          EXPECT_TRUE(X.extend(Y.combine(Z))
                          .semanticallyEqual(X.extend(Y).combine(X.extend(Z))));
          EXPECT_TRUE(X.combine(Y).extend(Z).semanticallyEqual(
              X.extend(Z).combine(Y.extend(Z))));
        }
      }
    }
    GenKillValue ConcreteContinuation = Seed;
    for (unsigned RuleIndex = 0; RuleIndex < 5; ++RuleIndex) {
      ConcreteContinuation =
          ConcreteContinuation.extend(Model.rules()[RuleIndex].weight);
    }
    DataFlowFacts ConcreteContinuationFacts =
        ConcreteContinuation.apply(DataFlowFacts::EmptySet());

    for (WPDSQueryKind Operation :
         {WPDSQueryKind::PostStar, WPDSQueryKind::PreStar}) {
      SCOPED_TRACE(::testing::Message() << "operation=" << toString(Operation));
      backend::Query Query;
      Query.operation = Operation;
      Query.initialState = Q;
      Query.roots = {Operation == WPDSQueryKind::PostStar ? Start : Done};
      Query.seed = Seed;
      std::string Error;
      auto Legacy =
          backend::prepareBackend(Model, {}, Operation, {Start, Entry}, Error);
      ASSERT_NE(Legacy, nullptr) << Error;
      backend::QueryResult Expected;
      ASSERT_TRUE(Legacy->solve(Query, Expected, Error)) << Error;
      if (Operation == WPDSQueryKind::PostStar) {
        DataFlowFacts LegacyContinuation =
            Expected.observations.at(Continuation)
                .summary.apply(DataFlowFacts::EmptySet());
        EXPECT_TRUE(
            DataFlowFacts::Eq(LegacyContinuation, ConcreteContinuationFacts))
            << "legacy disagrees with concrete execution at continuation";
      }

      for (WPDSBackendKind Backend :
           {WPDSBackendKind::WaliFWPDS, WPDSBackendKind::WaliSWPDS}) {
        SCOPED_TRACE(::testing::Message() << "backend=" << toString(Backend));
        auto Selected = backend::prepareBackend(
            Model, {Backend, false, false}, Operation, {Start, Entry}, Error);
        ASSERT_NE(Selected, nullptr) << Error;
        backend::QueryResult Actual;
        ASSERT_TRUE(Selected->solve(Query, Actual, Error)) << Error;
        for (backend::StackSymbolId Symbol = 1;
             Symbol < Model.stackSymbolNames().size(); ++Symbol) {
          const auto &Left = Actual.observations.at(Symbol);
          const auto &Right = Expected.observations.at(Symbol);
          EXPECT_EQ(Left.reachable, Right.reachable)
              << Model.stackSymbolName(Symbol);
          if (Left.reachable && Right.reachable) {
            DataFlowFacts LeftFacts =
                Left.summary.apply(DataFlowFacts::EmptySet());
            DataFlowFacts RightFacts =
                Right.summary.apply(DataFlowFacts::EmptySet());
            if (!DataFlowFacts::Eq(LeftFacts, RightFacts)) {
              std::ostringstream Details;
              LeftFacts.print(Details << " selected=");
              RightFacts.print(Details << " legacy=");
              Details << " selected_bits=" << LeftFacts.containsFact(A)
                      << LeftFacts.containsFact(B) << LeftFacts.containsFact(C)
                      << " legacy_bits=" << RightFacts.containsFact(A)
                      << RightFacts.containsFact(B)
                      << RightFacts.containsFact(C) << " rules=";
              for (const backend::Rule &Rule : Model.rules()) {
                Details << " [" << Rule.origin << " ";
                Rule.weight.print(Details);
                Details << "]";
              }
              ADD_FAILURE() << Model.stackSymbolName(Symbol) << Details.str();
            }
          }
        }
      }
    }
  }
}

TEST_F(WPDSTest, WaliBackendsMatchLegacyAcrossCallsAndRecursion) {
  for (WPDSBackendKind Backend :
       {WPDSBackendKind::WaliFWPDS, WPDSBackendKind::WaliSWPDS}) {
    for (bool Backward : {false, true}) {
      auto M = createRecursiveModule();
      Value *Seed = M->getNamedGlobal("seed");
      ASSERT_NE(Seed, nullptr);
      InterProceduralDataFlowEngine Engine({Backend, true, true});
      auto Prepared =
          Backward
              ? Engine.prepareBackwardAnalysis(
                    *M, [](Instruction *) { return GenKillTransformer::one(); })
              : Engine.prepareForwardAnalysis(*M, [](Instruction *) {
                  return GenKillTransformer::one();
                });
      ASSERT_NE(Prepared, nullptr) << Engine.getLastError();
      auto Result = Prepared->solveContextAggregated({Seed});
      ASSERT_NE(Result, nullptr) << Prepared->getLastError();
      auto *Call = findInstInModule(*M, "recursive_call");
      ASSERT_NE(Call, nullptr);
      EXPECT_TRUE(containsFact(Result->IN(Call), Seed));
      EXPECT_TRUE(containsFact(Result->OUT(Call), Seed));
    }
  }
}

TEST_F(WPDSTest, ExistingWPDSAnalysisClientsSelectEitherWaliBackend) {
  auto Compare = [](Module &M, const mono::DataFlowResult &Left,
                    const mono::DataFlowResult &Right) {
    for (Function &F : M) {
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          EXPECT_EQ(Left.IN(&I), Right.IN(&I));
          EXPECT_EQ(Left.OUT(&I), Right.OUT(&I));
          EXPECT_EQ(Left.GEN(&I), Right.GEN(&I));
          EXPECT_EQ(Left.KILL(&I), Right.KILL(&I));
        }
      }
    }
  };

  for (WPDSBackendKind Backend :
       {WPDSBackendKind::WaliFWPDS, WPDSBackendKind::WaliSWPDS}) {
    WPDSBackendOptions Options{Backend, true, true};
    {
      auto M = createLinearModule();
      auto Legacy = runLivenessAnalysis(*M);
      auto Selected = runLivenessAnalysis(*M, Options);
      ASSERT_NE(Selected, nullptr);
      Compare(*M, *Selected, *Legacy);
    }
    {
      auto M = createLinearModule();
      auto Legacy = runConstantPropagationAnalysis(*M);
      auto Selected = runConstantPropagationAnalysis(*M, Options);
      ASSERT_NE(Selected, nullptr);
      Compare(*M, *Selected, *Legacy);
    }
    {
      auto M = createLinearModule();
      auto Legacy = runTaintAnalysis(*M);
      auto Selected = runTaintAnalysis(*M, Options);
      ASSERT_NE(Selected, nullptr);
      Compare(*M, *Selected, *Legacy);
    }
    {
      auto M = createLinearModule();
      auto Legacy = runUninitializedVariablesAnalysis(*M);
      auto Selected = runUninitializedVariablesAnalysis(*M, Options);
      ASSERT_NE(Selected, nullptr);
      Compare(*M, *Selected, *Legacy);
    }
  }
}
#endif

TEST_F(WPDSTest, EngineStoresLocalGenKillInsteadOfPathSummaryEffects) {
  auto M = createLinearModule();
  InterProceduralDataFlowEngine Engine;
  auto *SeedFact = fact(11);

  auto Result = Engine.runForwardAnalysis(
      *M, [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "first") {
          return makeTransformer({}, {SeedFact});
        }
        return GenKillTransformer::one();
      });

  auto *First = findInstInModule(*M, "first");
  auto *Second = findInstInModule(*M, "second");
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);

  EXPECT_TRUE(containsFact(Result->GEN(First), SeedFact));
  EXPECT_TRUE(Result->KILL(First).empty());
  EXPECT_TRUE(Result->GEN(Second).empty());
  EXPECT_TRUE(Result->KILL(Second).empty());
  EXPECT_TRUE(containsFact(Result->IN(Second), SeedFact));
}

TEST_F(WPDSTest, MayJoinPreservesFactSeenOnOnlyOneBranch) {
  auto M = createBranchJoinModule();
  InterProceduralDataFlowEngine Engine;

  auto Result = Engine.runForwardAnalysis(
      *M, [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "then_fact") {
          return makeTransformer({}, {I});
        }
        return GenKillTransformer::one();
      });

  auto *ThenInst = findInstInModule(*M, "then_fact");
  auto *Ret = M->getFunction("main")->back().getTerminator();
  ASSERT_NE(ThenInst, nullptr);
  ASSERT_NE(Ret, nullptr);

  EXPECT_TRUE(containsFact(Result->OUT(ThenInst), ThenInst));
  EXPECT_TRUE(containsFact(Result->IN(Ret), ThenInst));
  EXPECT_TRUE(containsFact(Engine.getInSet(Ret), ThenInst));
}

TEST_F(WPDSTest, LivenessKillsUnnamedDefinitions) {
  auto M = createUnnamedLivenessModule();
  auto Result = runLivenessAnalysis(*M);
  ASSERT_NE(Result, nullptr);
  ASSERT_NE(UnnamedDef, nullptr);
  ASSERT_NE(RetInst, nullptr);

  EXPECT_TRUE(containsFact(Result->OUT(UnnamedDef), UnnamedDef));
  EXPECT_FALSE(containsFact(Result->IN(UnnamedDef), UnnamedDef));
  EXPECT_TRUE(containsFact(Result->IN(RetInst), UnnamedDef));
}

TEST_F(WPDSTest, LivenessTreatsStorePointerAsUseNotDefinition) {
  auto M = createStorePointerUseModule();
  auto Result = runLivenessAnalysis(*M);
  ASSERT_NE(Result, nullptr);
  ASSERT_NE(StoreInstForLiveness, nullptr);
  ASSERT_NE(PointerAllocaForLiveness, nullptr);
  ASSERT_NE(PtrUseInst, nullptr);

  EXPECT_TRUE(
      containsFact(Result->IN(StoreInstForLiveness), PointerAllocaForLiveness));
}

TEST_F(WPDSTest, UninitializedVariablesDocumentsAliasingLimitation) {
  auto M = createAliasLimitationModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Q = findInstInModule(*M, "q");
  auto *P = findInstInModule(*M, "p");
  auto *Load = findInstInModule(*M, "load_q");
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(Load, nullptr);

  EXPECT_FALSE(containsFact(Result->IN(Load), P));
}

TEST_F(WPDSTest, UninitializedVariablesTracksGlobalStoreLoadThroughObjectFact) {
  auto M = createGlobalStoreLoadModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Load = findInstInModule(*M, "load_g");
  auto *Global = M->getNamedGlobal("g");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Global, nullptr);

  EXPECT_FALSE(containsFact(Result->IN(Load), Global));
}

TEST_F(WPDSTest, UninitializedVariablesDoNotAssumeCallsInitializePointers) {
  auto M = createReadOnlyCallModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Load = findInstInModule(*M, "load_p");
  auto *P = findInstInModule(*M, "p");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(P, nullptr);

  EXPECT_TRUE(containsFact(Result->IN(Load), P));
}

TEST_F(WPDSTest, UninitializedVariablesPropagateLoadResultToUses) {
  auto M = createUninitializedLoadValueModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Loaded = findInstInModule(*M, "loaded");
  auto *Use = findInstInModule(*M, "use_loaded");
  ASSERT_NE(Loaded, nullptr);
  ASSERT_NE(Use, nullptr);

  EXPECT_TRUE(containsFact(Result->OUT(Loaded), Loaded));
  EXPECT_TRUE(containsFact(Result->IN(Use), Loaded));
}

TEST_F(WPDSTest, QueryHelpersExposeProgramPointFactsAndSummaries) {
  auto M = createLinearModule();
  InterProceduralDataFlowEngine Engine;
  auto *SeedFact = fact(21);

  auto Result = Engine.runForwardAnalysis(
      *M, [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "first") {
          return makeTransformer({}, {SeedFact});
        }
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *First = findInstInModule(*M, "first");
  auto *Second = findInstInModule(*M, "second");
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);

  EXPECT_NE(Engine.getProgramPointKeyAfterInstruction(First), WPDS_EPSILON);
  EXPECT_NE(Engine.getProgramPointKeyBeforeInstruction(Second), WPDS_EPSILON);
  EXPECT_TRUE(containsFact(Engine.queryFactsAfterInstruction(First), SeedFact));
  EXPECT_TRUE(
      containsFact(Engine.queryFactsBeforeInstruction(Second), SeedFact));
  auto Summary = Engine.querySummaryAfterInstruction(First);
  ASSERT_TRUE(Summary.get_ptr() != nullptr);
  EXPECT_FALSE(Summary->equal(GenKillTransformer::zero()));
}

TEST_F(WPDSTest, CallOutSetUsesAfterCallProgramPoint) {
  auto M = createReturnThroughCalleeModule();
  InterProceduralDataFlowEngine Engine;

  auto Result = Engine.runForwardAnalysis(
      *M, [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "seed") {
          return makeTransformer({}, {I});
        }
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *Call = findInstInModule(*M, "call_id");
  ASSERT_NE(Call, nullptr);

  EXPECT_EQ(Result->OUT(Call), Engine.queryFactsAfterInstruction(Call));
  EXPECT_TRUE(containsFact(Result->OUT(Call), Call));
}

TEST_F(WPDSTest, BackwardAnalysisMapsCalleeReturnBackToActual) {
  auto M = createReturnThroughCalleeModule();
  InterProceduralDataFlowEngine Engine;

  auto Result = Engine.runBackwardAnalysis(
      *M, [&](Instruction *I) -> GenKillTransformer * {
        if (auto *RI = dyn_cast<ReturnInst>(I)) {
          if (Value *RV = RI->getReturnValue()) {
            return makeTransformer({}, {RV});
          }
        }
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *Seed = findInstInModule(*M, "seed");
  auto *Call = findInstInModule(*M, "call_id");
  ASSERT_NE(Seed, nullptr);
  ASSERT_NE(Call, nullptr);

  EXPECT_TRUE(containsFact(Result->IN(Call), Seed));
  EXPECT_TRUE(containsFact(Engine.queryFactsBeforeInstruction(Call), Seed));
}

TEST_F(WPDSTest, UnknownCallPolicyCanSummarizeReturnPointerAndGlobalEffects) {
  auto M = createUnknownCallModule();
  InterProceduralDataFlowEngine Engine;
  InterProceduralDataFlowEngine::ExternalCallPolicy Policy;
  Policy.flowPointerArgumentsToReturn = false;
  Policy.flowGlobalsToReturn = false;
  Policy.buildSummary =
      [](CallBase *Call, const std::vector<Value *> &PointerObjects,
         const std::vector<GlobalValue *> &Globals) -> GenKillTransformer * {
    std::set<Value *> genSet;
    if (!PointerObjects.empty()) {
      genSet.insert(PointerObjects.front());
    }
    if (!Globals.empty()) {
      genSet.insert(Globals.front());
    }
    if (!Call->getType()->isVoidTy()) {
      genSet.insert(Call);
    }
    return GenKillTransformer::makeGenKillTransformer(DataFlowFacts::EmptySet(),
                                                      DataFlowFacts(genSet));
  };
  Engine.setExternalCallPolicy(Policy);

  auto Result =
      Engine.runForwardAnalysis(*M, [](Instruction *) -> GenKillTransformer * {
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *Call = findInstInModule(*M, "ext_result");
  auto *P = findInstInModule(*M, "p");
  auto *Global = M->getNamedGlobal("g");
  auto *Ret = M->getFunction("main")->back().getTerminator();
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Global, nullptr);
  ASSERT_NE(Ret, nullptr);

  EXPECT_TRUE(containsFact(Result->IN(Ret), Call));
  EXPECT_TRUE(containsFact(Result->IN(Ret), P));
  EXPECT_TRUE(containsFact(Result->IN(Ret), Global));
}

TEST_F(WPDSTest, UnknownCallPolicyCanDropIdentityWhileKeepingReturnSummary) {
  auto M = createUnknownCallModule();
  InterProceduralDataFlowEngine Engine;
  InterProceduralDataFlowEngine::ExternalCallPolicy Policy;
  Policy.preserveIdentity = false;
  Policy.flowPointerArgumentsToReturn = true;
  Policy.flowGlobalsToReturn = false;
  Engine.setExternalCallPolicy(Policy);

  auto *P = findInstInModule(*M, "p");
  ASSERT_NE(P, nullptr);

  auto Result =
      Engine.runForwardAnalysis(*M,
                                [](Instruction *) -> GenKillTransformer * {
                                  return GenKillTransformer::one();
                                },
                                {P});
  ASSERT_NE(Result, nullptr);

  auto *Call = findInstInModule(*M, "ext_result");
  auto *Ret = M->getFunction("main")->back().getTerminator();
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Ret, nullptr);

  EXPECT_TRUE(containsFact(Result->IN(Ret), Call));
  EXPECT_FALSE(containsFact(Result->IN(Ret), P));
}

TEST_F(WPDSTest, CustomResolverSupportsMultiCalleeMayJoin) {
  auto M = createMultiCalleeResolverModule();
  InterProceduralDataFlowEngine Engine;
  Engine.setCalleeResolver([&](CallBase *Call) -> std::vector<Function *> {
    if (Call->getCalledFunction() &&
        Call->getCalledFunction()->getName() == "dispatch") {
      return {M->getFunction("left"), M->getFunction("right")};
    }
    return {};
  });

  std::set<Value *> initialFacts = {M->getNamedGlobal("g1")};
  auto Result = Engine.runForwardAnalysis(
      *M,
      [](Instruction *) -> GenKillTransformer * {
        return GenKillTransformer::one();
      },
      initialFacts);
  ASSERT_NE(Result, nullptr);

  auto *Call = findInstInModule(*M, "dispatch_result");
  auto *After = findInstInModule(*M, "after_call");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(After, nullptr);
  EXPECT_TRUE(containsFact(Result->OUT(After), Call));
}

TEST_F(WPDSTest, MixedKnownAndUnknownCalleesDoNotReuseKnownReturnSummary) {
  auto M = createMixedCalleeResolverModule();
  InterProceduralDataFlowEngine Engine;
  InterProceduralDataFlowEngine::ExternalCallPolicy Policy;
  Policy.flowPointerArgumentsToReturn = false;
  Policy.flowGlobalsToReturn = true;
  Engine.setExternalCallPolicy(Policy);
  Engine.setCalleeResolver([&](CallBase *Call) -> std::vector<Function *> {
    if (Call->getCalledFunction() &&
        Call->getCalledFunction()->getName() == "dispatch") {
      return {M->getFunction("known"), nullptr};
    }
    return {};
  });

  std::set<Value *> initialFacts = {M->getNamedGlobal("known_g")};
  auto Result = Engine.runForwardAnalysis(
      *M,
      [](Instruction *) -> GenKillTransformer * {
        return GenKillTransformer::one();
      },
      initialFacts);
  ASSERT_NE(Result, nullptr);

  auto *Ret = M->getFunction("main")->back().getTerminator();
  auto *Call = findInstInModule(*M, "dispatch_result");
  auto *KnownGlobal = M->getNamedGlobal("known_g");
  auto *UnknownGlobal = M->getNamedGlobal("unknown_g");
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(KnownGlobal, nullptr);
  ASSERT_NE(UnknownGlobal, nullptr);

  EXPECT_TRUE(containsFact(Result->IN(Ret), Call));
  EXPECT_TRUE(containsFact(Result->IN(Ret), KnownGlobal));
  EXPECT_FALSE(containsFact(Result->IN(Ret), UnknownGlobal));
}

TEST_F(WPDSTest, ExplicitEntryAndExitSeedingRestrictAnalysisScope) {
  auto M = createTwoFunctionModule();
  InterProceduralDataFlowEngine Engine;
  auto *Seed = M->getNamedGlobal("seed");
  ASSERT_NE(Seed, nullptr);

  auto *MainInst = findInstInModule(*M, "main_inst");
  auto *HelperInst = findInstInModule(*M, "helper_inst");
  ASSERT_NE(MainInst, nullptr);
  ASSERT_NE(HelperInst, nullptr);

  auto Forward = Engine.runForwardAnalysisFromEntries(
      *M,
      [](Instruction *) -> GenKillTransformer * {
        return GenKillTransformer::one();
      },
      {M->getFunction("helper")}, {Seed});
  ASSERT_NE(Forward, nullptr);
  EXPECT_FALSE(containsFact(Forward->OUT(MainInst), Seed));
  EXPECT_TRUE(containsFact(Forward->OUT(HelperInst), Seed));

  auto Backward = Engine.runBackwardAnalysisFromExits(
      *M,
      [](Instruction *) -> GenKillTransformer * {
        return GenKillTransformer::one();
      },
      {M->getFunction("helper")}, {Seed});
  ASSERT_NE(Backward, nullptr);
  EXPECT_FALSE(containsFact(Backward->IN(MainInst), Seed));
  EXPECT_TRUE(containsFact(Backward->IN(HelperInst), Seed));
}

TEST_F(WPDSTest, ExplodedWPDSBuilderAddsRules) {
  wpds_key_t Lambda = str2key("Lambda");
  wpds_key_t N1 = str2key("n1");
  wpds_key_t N2 = str2key("n2");
  wpds_key_t Entry = str2key("entry");
  wpds_key_t Ret = str2key("ret");

  std::set<wpds_key_t> ControlStates = {Lambda, N1};
  std::vector<std::pair<wpds_key_t, wpds_key_t>> NormalEdges = {{Lambda, N1},
                                                                {N1, N2}};
  std::vector<std::tuple<wpds_key_t, wpds_key_t, wpds_key_t>> CallEdges = {
      std::make_tuple(N2, Entry, Ret)};

  Semiring<GenKillTransformer> Semiring(GenKillTransformer::one(), true);
  WPDS<GenKillTransformer> Wpds(Semiring, Query::poststar());

  auto GetNormal = [=](wpds_key_t FromC, wpds_key_t FromS, wpds_key_t ToC,
                       wpds_key_t ToS) -> GenKillTransformer * {
    if (FromC == Lambda && FromS == Lambda && ToC == Lambda && ToS == N1) {
      return GenKillTransformer::one();
    }
    if (FromC == Lambda && FromS == N1 && ToC == Lambda && ToS == N2) {
      return GenKillTransformer::one();
    }
    return nullptr;
  };

  auto GetCall = [=](wpds_key_t, wpds_key_t, wpds_key_t, wpds_key_t,
                     wpds_key_t) -> GenKillTransformer * {
    return GenKillTransformer::one();
  };

  buildExplodedWPDS<GenKillTransformer>(
      Wpds, Semiring, ControlStates, NormalEdges, CallEdges,
      std::function<GenKillTransformer *(wpds_key_t, wpds_key_t, wpds_key_t,
                                         wpds_key_t)>(GetNormal),
      std::function<GenKillTransformer *(wpds_key_t, wpds_key_t, wpds_key_t,
                                         wpds_key_t, wpds_key_t)>(GetCall));
  EXPECT_GE(Wpds.count_rules(), 1u);
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
