#include "Dataflow/WPDS/Clients/WPDSLivenessAnalysis.h"
#include "Dataflow/WPDS/Clients/WPDSUninitializedVariables.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <gtest/gtest.h>

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

  Value *fact(int N) {
    return ConstantInt::get(Type::getInt32Ty(*Ctx), N);
  }

  GenKillTransformer *makeTransformer(std::initializer_list<Value *> Kill,
                                      std::initializer_list<Value *> Gen,
                                      std::map<Value *, DataFlowFacts> Flow =
                                          {}) {
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

  std::unique_ptr<Module> createLinearModule() {
    auto M = std::make_unique<Module>("linear", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *First = BinaryOperator::CreateAdd(ConstantInt::get(I32, 1),
                                            ConstantInt::get(I32, 2), "first",
                                            Entry);
    auto *Second = BinaryOperator::CreateAdd(
        First, ConstantInt::get(I32, 3), "second", Entry);
    (void)Second;
    B.CreateRet(ConstantInt::get(I32, 0));
    return M;
  }

  std::unique_ptr<Module> createBranchJoinModule() {
    auto M = std::make_unique<Module>("branch_join", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());

    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    auto *ThenBB = BasicBlock::Create(*Ctx, "then", Main);
    auto *ElseBB = BasicBlock::Create(*Ctx, "else", Main);
    auto *MergeBB = BasicBlock::Create(*Ctx, "merge", Main);

    IRBuilder<> EntryBuilder(Entry);
    EntryBuilder.CreateCondBr(ConstantInt::getTrue(*Ctx), ThenBB, ElseBB);

    IRBuilder<> ThenBuilder(ThenBB);
    BinaryOperator::CreateAdd(ConstantInt::get(I32, 4), ConstantInt::get(I32, 5),
                              "then_fact", ThenBB);
    ThenBuilder.CreateBr(MergeBB);

    IRBuilder<> ElseBuilder(ElseBB);
    ElseBuilder.CreateBr(MergeBB);

    IRBuilder<> MergeBuilder(MergeBB);
    MergeBuilder.CreateRet(ConstantInt::get(I32, 0));
    return M;
  }

  std::unique_ptr<Module> createAliasLimitationModule() {
    auto M = std::make_unique<Module>("alias_limitation", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);

    auto *P = B.CreateAlloca(I32, nullptr, "p");
    auto *Q = cast<Instruction>(B.CreateGEP(
        I32, P, ConstantInt::get(Type::getInt32Ty(*Ctx), 0), "q"));
    B.CreateStore(ConstantInt::get(I32, 1), P);
    auto *Load = B.CreateLoad(I32, Q, "load_q");
    B.CreateRet(Load);
    return M;
  }

  std::unique_ptr<Module> createGlobalStoreLoadModule() {
    auto M = std::make_unique<Module>("global_store_load", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *Global = new GlobalVariable(
        *M, I32, false, GlobalValue::ExternalLinkage,
        ConstantInt::get(I32, 0), "g");
    (void)Global;

    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *Store = B.CreateStore(ConstantInt::get(I32, 9), M->getNamedGlobal("g"));
    (void)Store;
    auto *Load = B.CreateLoad(I32, M->getNamedGlobal("g"), "load_g");
    B.CreateRet(Load);
    return M;
  }

  std::unique_ptr<Module> createUnknownCallModule() {
    auto M = std::make_unique<Module>("unknown_call", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *PtrTy = PointerType::getUnqual(I32);
    new GlobalVariable(*M, I32, false, GlobalValue::ExternalLinkage,
                       ConstantInt::get(I32, 0), "g");

    auto *ExtTy = FunctionType::get(I32, {PtrTy}, false);
    auto *Ext =
        Function::Create(ExtTy, Function::ExternalLinkage, "ext", M.get());

    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *P = B.CreateAlloca(I32, nullptr, "p");
    auto *Call = B.CreateCall(ExtTy, Ext, {P}, "ext_result");
    B.CreateRet(Call);
    return M;
  }

  std::unique_ptr<Module> createReadOnlyCallModule() {
    auto M = std::make_unique<Module>("readonly_call", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *PtrTy = PointerType::getUnqual(I32);

    auto *ReaderTy = FunctionType::get(I32, {PtrTy}, false);
    auto *Reader = Function::Create(ReaderTy, Function::ExternalLinkage,
                                    "reader", M.get());

    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *P = B.CreateAlloca(I32, nullptr, "p");
    auto *Call = B.CreateCall(ReaderTy, Reader, {P}, "reader_result");
    (void)Call;
    auto *Load = B.CreateLoad(I32, P, "load_p");
    B.CreateRet(Load);
    return M;
  }

  std::unique_ptr<Module> createMultiCalleeResolverModule() {
    auto M = std::make_unique<Module>("multi_callee", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *PtrTy = PointerType::getUnqual(I32);
    new GlobalVariable(*M, I32, false, GlobalValue::ExternalLinkage,
                       ConstantInt::get(I32, 1), "g1");
    new GlobalVariable(*M, I32, false, GlobalValue::ExternalLinkage,
                       ConstantInt::get(I32, 2), "g2");

    auto *DispatchTy = FunctionType::get(PtrTy, {}, false);
    auto *Dispatch = Function::Create(DispatchTy, Function::ExternalLinkage,
                                      "dispatch", M.get());

    auto *Left = Function::Create(DispatchTy, Function::InternalLinkage, "left",
                                  M.get());
    auto *LeftBB = BasicBlock::Create(*Ctx, "entry", Left);
    IRBuilder<> LeftBuilder(LeftBB);
    LeftBuilder.CreateRet(M->getNamedGlobal("g1"));

    auto *Right = Function::Create(DispatchTy, Function::InternalLinkage, "right",
                                   M.get());
    auto *RightBB = BasicBlock::Create(*Ctx, "entry", Right);
    IRBuilder<> RightBuilder(RightBB);
    RightBuilder.CreateRet(M->getNamedGlobal("g2"));

    auto *Main = Function::Create(DispatchTy, Function::ExternalLinkage, "main",
                                  M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *Call = B.CreateCall(DispatchTy, Dispatch, {}, "dispatch_result");
    auto *After = B.CreatePtrToInt(Call, I32, "after_call");
    B.CreateRet(After);
    return M;
  }

  std::unique_ptr<Module> createMixedCalleeResolverModule() {
    auto M = std::make_unique<Module>("mixed_callee", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *PtrTy = PointerType::getUnqual(I32);
    auto *KnownGlobal = new GlobalVariable(
        *M, I32, false, GlobalValue::ExternalLinkage,
        ConstantInt::get(I32, 1), "known_g");
    new GlobalVariable(*M, I32, false, GlobalValue::ExternalLinkage,
                       ConstantInt::get(I32, 2), "unknown_g");

    auto *DispatchTy = FunctionType::get(PtrTy, {PtrTy}, false);
    auto *Dispatch = Function::Create(DispatchTy, Function::ExternalLinkage,
                                      "dispatch", M.get());

    auto *Known = Function::Create(DispatchTy, Function::InternalLinkage,
                                   "known", M.get());
    Known->arg_begin()->setName("arg");
    auto *KnownBB = BasicBlock::Create(*Ctx, "entry", Known);
    IRBuilder<> KnownBuilder(KnownBB);
    KnownBuilder.CreateRet(KnownGlobal);

    auto *Main = Function::Create(DispatchTy, Function::ExternalLinkage, "main",
                                  M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *P = B.CreateAlloca(I32, nullptr, "p");
    auto *Call = B.CreateCall(DispatchTy, Dispatch, {P}, "dispatch_result");
    auto *After = B.CreatePtrToInt(Call, I32, "after_call");
    B.CreateRet(After);
    return M;
  }

  std::unique_ptr<Module> createTwoFunctionModule() {
    auto M = std::make_unique<Module>("two_functions", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    new GlobalVariable(*M, I32, false, GlobalValue::ExternalLinkage,
                       ConstantInt::get(I32, 1), "seed");

    auto *FnTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(FnTy, Function::ExternalLinkage, "main", M.get());
    auto *MainBB = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> MainBuilder(MainBB);
    auto *MainInst = BinaryOperator::CreateAdd(ConstantInt::get(I32, 1),
                                               ConstantInt::get(I32, 2),
                                               "main_inst", MainBB);
    (void)MainInst;
    MainBuilder.CreateRet(ConstantInt::get(I32, 0));

    auto *Helper =
        Function::Create(FnTy, Function::InternalLinkage, "helper", M.get());
    auto *HelperBB = BasicBlock::Create(*Ctx, "entry", Helper);
    IRBuilder<> HelperBuilder(HelperBB);
    auto *HelperInst = BinaryOperator::CreateAdd(ConstantInt::get(I32, 3),
                                                 ConstantInt::get(I32, 4),
                                                 "helper_inst", HelperBB);
    (void)HelperInst;
    HelperBuilder.CreateRet(ConstantInt::get(I32, 0));
    return M;
  }

  std::unique_ptr<Module> createReturnThroughCalleeModule() {
    auto M = std::make_unique<Module>("return_through_callee", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);

    auto *CalleeTy = FunctionType::get(I32, {I32}, false);
    auto *Callee = Function::Create(CalleeTy, Function::InternalLinkage, "id",
                                    M.get());
    Callee->arg_begin()->setName("arg");
    auto *CalleeBB = BasicBlock::Create(*Ctx, "entry", Callee);
    IRBuilder<> CalleeBuilder(CalleeBB);
    CalleeBuilder.CreateRet(&*Callee->arg_begin());

    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *Seed = BinaryOperator::CreateAdd(ConstantInt::get(I32, 1),
                                           ConstantInt::get(I32, 2), "seed",
                                           Entry);
    auto *Call = B.CreateCall(CalleeTy, Callee, {Seed}, "call_id");
    B.CreateRet(Call);
    (void)Call;
    return M;
  }

  std::unique_ptr<Module> createUninitializedLoadValueModule() {
    auto M = std::make_unique<Module>("uninit_load_value", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *P = B.CreateAlloca(I32, nullptr, "p");
    auto *Loaded = B.CreateLoad(I32, P, "loaded");
    auto *Use = B.CreateAdd(Loaded, ConstantInt::get(I32, 1), "use_loaded");
    B.CreateRet(Use);
    return M;
  }

  std::unique_ptr<Module> createUnnamedLivenessModule() {
    auto M = std::make_unique<Module>("liveness", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *Tmp = BinaryOperator::CreateAdd(ConstantInt::get(I32, 1),
                                          ConstantInt::get(I32, 2), "", Entry);
    auto *Ret = B.CreateRet(Tmp);
    UnnamedDef = Tmp;
    RetInst = Ret;
    return M;
  }

  std::unique_ptr<Module> createStorePointerUseModule() {
    auto M = std::make_unique<Module>("store_pointer_use", *Ctx);
    auto *I32 = Type::getInt32Ty(*Ctx);
    auto *MainTy = FunctionType::get(I32, {}, false);
    auto *Main =
        Function::Create(MainTy, Function::ExternalLinkage, "main", M.get());
    auto *Entry = BasicBlock::Create(*Ctx, "entry", Main);
    IRBuilder<> B(Entry);
    auto *P = B.CreateAlloca(I32, nullptr, "p");
    auto *Store = B.CreateStore(ConstantInt::get(I32, 1), P);
    auto *Cast = B.CreatePtrToInt(P, Type::getInt64Ty(*Ctx), "ptr_use");
    auto *Trunc = B.CreateTrunc(Cast, I32, "ptr_use32");
    (void)Trunc;
    B.CreateRet(ConstantInt::get(I32, 0));
    StoreInstForLiveness = cast<StoreInst>(Store);
    PointerAllocaForLiveness = P;
    PtrUseInst = cast<Instruction>(Cast);
    return M;
  }

  Instruction *findInstructionByName(Module &M, StringRef Name) {
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
  EXPECT_TRUE(GenKillTransformer::zero()->extend(T1)
                  ->equal(GenKillTransformer::zero()));
  EXPECT_TRUE(T1->combine(GenKillTransformer::zero())->equal(T1));
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
      *M,
      [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "first") {
          return makeTransformer({}, {SeedFact});
        }
        return GenKillTransformer::one();
      });

  auto *First = findInstructionByName(*M, "first");
  auto *Second = findInstructionByName(*M, "second");
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);

  EXPECT_EQ(Result->OUT(First), Engine.getOutSet(First));
  EXPECT_EQ(Result->IN(Second), Engine.getInSet(Second));
  EXPECT_TRUE(containsFact(Result->OUT(First), SeedFact));
}

TEST_F(WPDSTest, EngineStoresLocalGenKillInsteadOfPathSummaryEffects) {
  auto M = createLinearModule();
  InterProceduralDataFlowEngine Engine;
  auto *SeedFact = fact(11);

  auto Result = Engine.runForwardAnalysis(
      *M,
      [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "first") {
          return makeTransformer({}, {SeedFact});
        }
        return GenKillTransformer::one();
      });

  auto *First = findInstructionByName(*M, "first");
  auto *Second = findInstructionByName(*M, "second");
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
      *M,
      [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "then_fact") {
          return makeTransformer({}, {I});
        }
        return GenKillTransformer::one();
      });

  auto *ThenInst = findInstructionByName(*M, "then_fact");
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

  EXPECT_TRUE(containsFact(Result->IN(StoreInstForLiveness),
                           PointerAllocaForLiveness));
}

TEST_F(WPDSTest, UninitializedVariablesDocumentsAliasingLimitation) {
  auto M = createAliasLimitationModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Q = findInstructionByName(*M, "q");
  auto *P = findInstructionByName(*M, "p");
  auto *Load = findInstructionByName(*M, "load_q");
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(Load, nullptr);

  EXPECT_FALSE(containsFact(Result->IN(Load), P));
}

TEST_F(WPDSTest, UninitializedVariablesTracksGlobalStoreLoadThroughObjectFact) {
  auto M = createGlobalStoreLoadModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Load = findInstructionByName(*M, "load_g");
  auto *Global = M->getNamedGlobal("g");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Global, nullptr);

  EXPECT_FALSE(containsFact(Result->IN(Load), Global));
}

TEST_F(WPDSTest, UninitializedVariablesDoNotAssumeCallsInitializePointers) {
  auto M = createReadOnlyCallModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Load = findInstructionByName(*M, "load_p");
  auto *P = findInstructionByName(*M, "p");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(P, nullptr);

  EXPECT_TRUE(containsFact(Result->IN(Load), P));
}

TEST_F(WPDSTest, UninitializedVariablesPropagateLoadResultToUses) {
  auto M = createUninitializedLoadValueModule();
  auto Result = runUninitializedVariablesAnalysis(*M);
  ASSERT_NE(Result, nullptr);

  auto *Loaded = findInstructionByName(*M, "loaded");
  auto *Use = findInstructionByName(*M, "use_loaded");
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
      *M,
      [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "first") {
          return makeTransformer({}, {SeedFact});
        }
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *First = findInstructionByName(*M, "first");
  auto *Second = findInstructionByName(*M, "second");
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);

  EXPECT_NE(Engine.getProgramPointKeyAfterInstruction(First), WPDS_EPSILON);
  EXPECT_NE(Engine.getProgramPointKeyBeforeInstruction(Second), WPDS_EPSILON);
  EXPECT_TRUE(containsFact(Engine.queryFactsAfterInstruction(First), SeedFact));
  EXPECT_TRUE(containsFact(Engine.queryFactsBeforeInstruction(Second), SeedFact));
  auto Summary = Engine.querySummaryAfterInstruction(First);
  ASSERT_TRUE(Summary.get_ptr() != nullptr);
  EXPECT_FALSE(Summary->equal(GenKillTransformer::zero()));
}

TEST_F(WPDSTest, CallOutSetUsesAfterCallProgramPoint) {
  auto M = createReturnThroughCalleeModule();
  InterProceduralDataFlowEngine Engine;

  auto Result = Engine.runForwardAnalysis(
      *M,
      [&](Instruction *I) -> GenKillTransformer * {
        if (I->getName() == "seed") {
          return makeTransformer({}, {I});
        }
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *Call = findInstructionByName(*M, "call_id");
  ASSERT_NE(Call, nullptr);

  EXPECT_EQ(Result->OUT(Call), Engine.queryFactsAfterInstruction(Call));
  EXPECT_TRUE(containsFact(Result->OUT(Call), Call));
}

TEST_F(WPDSTest, BackwardAnalysisMapsCalleeReturnBackToActual) {
  auto M = createReturnThroughCalleeModule();
  InterProceduralDataFlowEngine Engine;

  auto Result = Engine.runBackwardAnalysis(
      *M,
      [&](Instruction *I) -> GenKillTransformer * {
        if (auto *RI = dyn_cast<ReturnInst>(I)) {
          if (Value *RV = RI->getReturnValue()) {
            return makeTransformer({}, {RV});
          }
        }
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *Seed = findInstructionByName(*M, "seed");
  auto *Call = findInstructionByName(*M, "call_id");
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
    return GenKillTransformer::makeGenKillTransformer(
        DataFlowFacts::EmptySet(), DataFlowFacts(genSet));
  };
  Engine.setExternalCallPolicy(Policy);

  auto Result = Engine.runForwardAnalysis(
      *M, [](Instruction *) -> GenKillTransformer * {
        return GenKillTransformer::one();
      });
  ASSERT_NE(Result, nullptr);

  auto *Call = findInstructionByName(*M, "ext_result");
  auto *P = findInstructionByName(*M, "p");
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

  auto *P = findInstructionByName(*M, "p");
  ASSERT_NE(P, nullptr);

  auto Result = Engine.runForwardAnalysis(
      *M,
      [](Instruction *) -> GenKillTransformer * {
        return GenKillTransformer::one();
      },
      {P});
  ASSERT_NE(Result, nullptr);

  auto *Call = findInstructionByName(*M, "ext_result");
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

  auto *Call = findInstructionByName(*M, "dispatch_result");
  auto *After = findInstructionByName(*M, "after_call");
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
  auto *Call = findInstructionByName(*M, "dispatch_result");
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

  auto *MainInst = findInstructionByName(*M, "main_inst");
  auto *HelperInst = findInstructionByName(*M, "helper_inst");
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
  std::vector<std::pair<wpds_key_t, wpds_key_t>> NormalEdges = {
      {Lambda, N1}, {N1, N2}};
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
