#include <Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h>
#include <Dataflow/IFDS/Clients/IFDSConstAnalysis.h>
#include <Dataflow/IFDS/Clients/IFDSReachingDefinitions.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>

#include <gtest/gtest.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <memory>

namespace {

struct InternalCallIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::Instruction *AfterCall = nullptr;
  llvm::Instruction *AllocaInst = nullptr;
  llvm::Instruction *CalleeRetInst = nullptr;
  llvm::CallInst *Call = nullptr;
};

InternalCallIR buildInternalCallIR() {
  InternalCallIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = std::make_unique<llvm::Module>("ifds_solver_alias_internal", *IR.Ctx);

  auto *I8Ty = llvm::Type::getInt8Ty(*IR.Ctx);
  auto *I8PtrTy = llvm::Type::getInt8PtrTy(*IR.Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*IR.Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*IR.Ctx);

  auto *CalleeTy = llvm::FunctionType::get(I8PtrTy, {I8PtrTy}, false);
  auto *Callee = llvm::Function::Create(CalleeTy, llvm::Function::ExternalLinkage,
                                        "callee", IR.Mod.get());
  auto *CalleeEntry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Callee);
  llvm::IRBuilder<> CB(CalleeEntry);
  IR.CalleeRetInst = CB.CreateRet(Callee->getArg(0));

  auto *MainTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                      "main", IR.Mod.get());
  auto *Entry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);

  auto *Alloca = B.CreateAlloca(I8Ty, nullptr, "local");
  IR.AllocaInst = Alloca;

  IR.Call = B.CreateCall(Callee, {Alloca});
  IR.AfterCall = llvm::cast<llvm::Instruction>(
      B.CreatePtrToInt(IR.Call, I64Ty, "after_call"));

  B.CreateRet(llvm::ConstantInt::get(I32Ty, 0));

  return IR;
}

struct ExternalCallIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::Instruction *AfterExtCall = nullptr;
  llvm::Instruction *GlobalStore = nullptr;
  const llvm::GlobalVariable *Global = nullptr;
};

ExternalCallIR buildExternalCallIR() {
  ExternalCallIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = std::make_unique<llvm::Module>("ifds_solver_alias_external", *IR.Ctx);

  auto *I8Ty = llvm::Type::getInt8Ty(*IR.Ctx);
  auto *I8PtrTy = llvm::Type::getInt8PtrTy(*IR.Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*IR.Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*IR.Ctx);

  IR.Global = new llvm::GlobalVariable(
      *IR.Mod, I8Ty, false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(I8Ty, 0), "g");

  auto *ExtTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*IR.Ctx), {I8PtrTy}, false);
  auto *Ext = llvm::Function::Create(ExtTy, llvm::Function::ExternalLinkage, "ext", IR.Mod.get());

  auto *MainTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                      "main", IR.Mod.get());
  auto *Entry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);

  auto *Alloca = B.CreateAlloca(I8Ty, nullptr, "local");
  IR.GlobalStore = B.CreateStore(llvm::ConstantInt::get(I8Ty, 1),
                                 const_cast<llvm::GlobalVariable *>(IR.Global));
  B.CreateCall(Ext, {Alloca});
  IR.AfterExtCall = llvm::cast<llvm::Instruction>(
      B.CreatePtrToInt(Alloca, I64Ty, "after_ext_call"));
  B.CreateRet(llvm::ConstantInt::get(I32Ty, 0));

  return IR;
}

struct TaintAliasIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::Instruction *LoadInst = nullptr;
  llvm::CallBase *SinkCall = nullptr;
};

TaintAliasIR buildTaintAliasIR() {
  TaintAliasIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = std::make_unique<llvm::Module>("ifds_solver_alias_taint", *IR.Ctx);

  auto *I8Ty = llvm::Type::getInt8Ty(*IR.Ctx);
  auto *I8PtrTy = llvm::Type::getInt8PtrTy(*IR.Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*IR.Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*IR.Ctx);

  auto *SourceTy = llvm::FunctionType::get(I8Ty, {}, false);
  auto *SinkTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*IR.Ctx),
                                         {I8Ty}, false);
  auto *Source = llvm::Function::Create(SourceTy, llvm::Function::ExternalLinkage,
                                        "source", IR.Mod.get());
  auto *Sink = llvm::Function::Create(SinkTy, llvm::Function::ExternalLinkage,
                                      "sink", IR.Mod.get());

  auto *MainTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                      "main", IR.Mod.get());
  auto *Entry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);

  auto *Alloca = B.CreateAlloca(I8Ty, nullptr, "p");
  auto *AliasPtr =
      B.CreateGEP(I8Ty, Alloca, llvm::ConstantInt::get(I64Ty, 0), "q");
  auto *SourceCall = B.CreateCall(Source, {});
  B.CreateStore(SourceCall, Alloca);
  IR.LoadInst = B.CreateLoad(I8Ty, AliasPtr, "loaded");
  IR.SinkCall = B.CreateCall(Sink, {IR.LoadInst});
  B.CreateRet(llvm::ConstantInt::get(I32Ty, 0));

  return IR;
}

struct ConstAliasIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::StoreInst *SecondStore = nullptr;
  llvm::Value *AliasPtr = nullptr;
};

ConstAliasIR buildConstAliasIR() {
  ConstAliasIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = std::make_unique<llvm::Module>("ifds_solver_alias_const", *IR.Ctx);

  auto *I8Ty = llvm::Type::getInt8Ty(*IR.Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*IR.Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*IR.Ctx);

  auto *MainTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                      "main", IR.Mod.get());
  auto *Entry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);

  auto *Alloca = B.CreateAlloca(I8Ty, nullptr, "p");
  IR.AliasPtr =
      B.CreateGEP(I8Ty, Alloca, llvm::ConstantInt::get(I64Ty, 0), "q");
  B.CreateStore(llvm::ConstantInt::get(I8Ty, 1), Alloca);
  IR.SecondStore = B.CreateStore(llvm::ConstantInt::get(I8Ty, 2), IR.AliasPtr);
  B.CreateRet(llvm::ConstantInt::get(I32Ty, 0));

  return IR;
}

bool containsDefinition(
    const ifds::ReachingDefinitionsAnalysis::FactSet &Facts,
    const llvm::Value *Var, const llvm::Instruction *Site) {
  for (const auto &Fact : Facts) {
    if (Fact.is_definition() && Fact.get_variable() == Var &&
        Fact.get_definition_site() == Site) {
      return true;
    }
  }
  return false;
}

bool containsAnyDefinitionFor(
    const ifds::ReachingDefinitionsAnalysis::FactSet &Facts,
    const llvm::Value *Var) {
  for (const auto &Fact : Facts) {
    if (Fact.is_definition() && Fact.get_variable() == Var) {
      return true;
    }
  }
  return false;
}

bool containsTaintedVar(const ifds::TaintAnalysis::FactSet &Facts,
                        const llvm::Value *Val) {
  for (const auto &Fact : Facts) {
    if (Fact.is_tainted_var() && Fact.get_value() == Val) {
      return true;
    }
  }
  return false;
}

bool containsConstFact(const ifds::ConstAnalysis::FactSet &Facts,
                       const ifds::ConstFact &Expected) {
  return Facts.count(Expected) > 0;
}

ifds::TaintAnalysis::FactSet
solveTaintAliasScenario(const llvm::Module &Mod,
                        lotus::AliasAnalysisWrapper *AA = nullptr,
                        bool AutoInject = false) {
  ifds::TaintAnalysis Analysis;
  Analysis.add_source_function("source");
  Analysis.add_sink_function("sink");
  if (AA != nullptr) {
    Analysis.set_alias_analysis(AA);
  }

  ifds::IFDSSolver<ifds::TaintAnalysis> Solver(Analysis);
  if (AutoInject) {
    auto Config = Solver.get_solver_config();
    Config.set_auto_inject_alias_analysis(true);
    Solver.set_solver_config(Config);
  }
  Solver.solve(Mod);

  const llvm::Function *Main = Mod.getFunction("main");
  const llvm::CallBase *SinkCall = nullptr;
  const llvm::Instruction *LoadInst = nullptr;
  for (const auto &Inst : Main->getEntryBlock()) {
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
      LoadInst = Load;
    }
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
      if (Call->getCalledFunction() &&
          Call->getCalledFunction()->getName() == "sink") {
        SinkCall = Call;
      }
    }
  }
  EXPECT_NE(LoadInst, nullptr);
  EXPECT_NE(SinkCall, nullptr);
  return Solver.get_facts_at_entry(SinkCall);
}

ifds::ConstAnalysis::FactSet
solveConstAliasScenario(const llvm::Module &Mod, lotus::AliasAnalysisWrapper &AA,
                        const llvm::Instruction *SecondStore) {
  ifds::ConstAnalysis Analysis(&AA);
  ifds::IFDSSolver<ifds::ConstAnalysis> Solver(Analysis);
  Solver.solve(Mod);
  return Solver.get_facts_at_exit(SecondStore);
}

} // namespace

TEST(IFDSSolverAliasIntegrationTest,
     InternalCallPropagatesReturnDefAndPreservesCallerLocalDef) {
  auto IR = buildInternalCallIR();
  ifds::ReachingDefinitionsAnalysis Analysis;
  ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> Solver(Analysis);

  Solver.solve(*IR.Mod);

  auto FactsAtAfterCall = Solver.get_facts_at_entry(IR.AfterCall);

  EXPECT_TRUE(containsDefinition(FactsAtAfterCall, IR.Call, IR.CalleeRetInst));
  EXPECT_TRUE(containsAnyDefinitionFor(FactsAtAfterCall, IR.AllocaInst));
}

TEST(IFDSSolverAliasIntegrationTest, ExternalCallKillsGlobalDefinition) {
  auto IR = buildExternalCallIR();
  ifds::ReachingDefinitionsAnalysis Analysis;
  ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> Solver(Analysis);

  Solver.solve(*IR.Mod);

  auto FactsAfterExt = Solver.get_facts_at_entry(IR.AfterExtCall);

  EXPECT_FALSE(containsDefinition(FactsAfterExt, IR.Global, IR.GlobalStore));
  EXPECT_FALSE(containsAnyDefinitionFor(FactsAfterExt, IR.Global));
}

TEST(IFDSSolverAliasIntegrationTest,
     AutoInjectedAliasDefaultPropagatesTaintAcrossPointerAliases) {
  auto IR = buildTaintAliasIR();
  auto FactsAtSink = solveTaintAliasScenario(*IR.Mod, nullptr, true);

  EXPECT_TRUE(containsTaintedVar(FactsAtSink, IR.LoadInst));
}

TEST(IFDSSolverAliasIntegrationTest,
     ExplicitSparrowAndDyckPropagateTaintAcrossPointerAliases) {
  auto IR = buildTaintAliasIR();

  lotus::AliasAnalysisWrapper SparrowAA(*IR.Mod, lotus::AAConfig::SparrowAA_NoCtx());
  auto SparrowFacts = solveTaintAliasScenario(*IR.Mod, &SparrowAA, false);
  EXPECT_TRUE(containsTaintedVar(SparrowFacts, IR.LoadInst));

  lotus::AliasAnalysisWrapper DyckAA(*IR.Mod, lotus::AAConfig::DyckAA());
  auto DyckFacts = solveTaintAliasScenario(*IR.Mod, &DyckAA, false);
  EXPECT_TRUE(containsTaintedVar(DyckFacts, IR.LoadInst));
}

TEST(IFDSSolverAliasIntegrationTest,
     ExplicitSparrowAndDyckMarkSecondAliasWriteAsMutable) {
  auto IR = buildConstAliasIR();

  lotus::AliasAnalysisWrapper SparrowAA(*IR.Mod, lotus::AAConfig::SparrowAA_NoCtx());
  auto SparrowFacts = solveConstAliasScenario(*IR.Mod, SparrowAA, IR.SecondStore);
  EXPECT_TRUE(
      containsConstFact(SparrowFacts, ifds::ConstFact::mutable_mem(IR.AliasPtr)));

  lotus::AliasAnalysisWrapper DyckAA(*IR.Mod, lotus::AAConfig::DyckAA());
  auto DyckFacts = solveConstAliasScenario(*IR.Mod, DyckAA, IR.SecondStore);
  EXPECT_TRUE(
      containsConstFact(DyckFacts, ifds::ConstFact::mutable_mem(IR.AliasPtr)));
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
