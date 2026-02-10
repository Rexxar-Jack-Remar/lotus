#include <Dataflow/IFDS/Clients/IFDSReachingDefinitions.h>
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

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
