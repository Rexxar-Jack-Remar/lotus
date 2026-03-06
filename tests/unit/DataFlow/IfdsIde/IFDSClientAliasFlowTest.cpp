#include <Dataflow/IFDS/Clients/IFDSConstAnalysis.h>
#include <Dataflow/IFDS/Clients/IFDSReachingDefinitions.h>

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

struct IFDSFlowFixtureIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  const llvm::Function *Callee = nullptr;
  const llvm::CallBase*Call = nullptr;
  const llvm::CallBase*ExtCall = nullptr;
  const llvm::Instruction *CallReturnSite = nullptr;
  const llvm::Instruction *ExtCallReturnSite = nullptr;
  llvm::Argument *Formal = nullptr;
  const llvm::Value *Actual = nullptr;
  const llvm::Instruction *CalleeEntryInst = nullptr;
  const llvm::GlobalVariable *Global = nullptr;
};

IFDSFlowFixtureIR buildIFDSFlowFixture() {
  IFDSFlowFixtureIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = std::make_unique<llvm::Module>("ifds_client_alias_flow_test", *IR.Ctx);

  auto *I8Ty = llvm::Type::getInt8Ty(*IR.Ctx);
  auto *I8PtrTy = llvm::Type::getInt8PtrTy(*IR.Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*IR.Ctx);

  IR.Global = new llvm::GlobalVariable(
      *IR.Mod, I8Ty, false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(I8Ty, 0), "g");

  auto *CalleeTy = llvm::FunctionType::get(I8PtrTy, {I8PtrTy}, false);
  auto *Callee = llvm::Function::Create(CalleeTy, llvm::Function::ExternalLinkage,
                                        "callee", IR.Mod.get());
  IR.Callee = Callee;
  IR.Formal = &*Callee->arg_begin();

  auto *CalleeEntry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Callee);
  llvm::IRBuilder<> CB(CalleeEntry);
  IR.CalleeEntryInst = CB.CreateRet(IR.Formal);

  auto *ExtTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*IR.Ctx), {I8PtrTy}, false);
  auto *Ext = llvm::Function::Create(ExtTy, llvm::Function::ExternalLinkage, "ext", IR.Mod.get());

  auto *MainTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                      "main", IR.Mod.get());
  auto *Entry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);
  auto *Alloca = B.CreateAlloca(I8Ty, nullptr, "actual");
  IR.Actual = Alloca;
  IR.Call = B.CreateCall(Callee, {Alloca});
  IR.ExtCall = B.CreateCall(Ext, {Alloca});
  auto *Ret = B.CreateRet(llvm::ConstantInt::get(I32Ty, 0));
  IR.CallReturnSite = IR.ExtCall;
  IR.ExtCallReturnSite = Ret;

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

} // namespace

TEST(IFDSConstAnalysisFlowTest, CallFlowMapsActualToFormal) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto Out = Analysis.call_flow(IR.Call, IR.Callee,
                                ifds::ConstFact::initialized(IR.Actual));

  EXPECT_EQ(Out.count(ifds::ConstFact::initialized(IR.Formal)), 1U);
}

TEST(IFDSConstAnalysisFlowTest, ReturnFlowMapsFormalBackToActual) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto Out = Analysis.return_flow(IR.Call, IR.CallReturnSite, IR.Callee,
                                  ifds::ConstFact::mutable_mem(IR.Formal),
                                  ifds::ConstFact::zero());

  EXPECT_EQ(Out.count(ifds::ConstFact::mutable_mem(IR.Actual)), 1U);
}

TEST(IFDSConstAnalysisFlowTest, CallToReturnKillsPointerArgFact) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto Out = Analysis.call_to_return_flow(
      IR.Call, IR.CallReturnSite, ifds::ConstFact::initialized(IR.Actual));

  EXPECT_TRUE(Out.empty());
}

TEST(IFDSConstAnalysisFlowTest, CallToReturnPreservesZeroAndGlobalFacts) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto ZeroOut =
      Analysis.call_to_return_flow(IR.Call, IR.CallReturnSite,
                                   ifds::ConstFact::zero());
  EXPECT_EQ(ZeroOut.count(ifds::ConstFact::zero()), 1U);

  auto GlobalOut = Analysis.call_to_return_flow(
      IR.Call, IR.CallReturnSite, ifds::ConstFact::initialized(IR.Global));
  EXPECT_EQ(GlobalOut.count(ifds::ConstFact::initialized(IR.Global)), 1U);
}

TEST(IFDSReachingDefinitionsFlowTest, CallFlowMapsActualDefToFormalDef) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto Out = Analysis.call_flow(
      IR.Call, IR.Callee,
      ifds::DefinitionFact::definition(IR.Actual, IR.Call));

  EXPECT_TRUE(containsDefinition(Out, IR.Formal, IR.CalleeEntryInst));
}

TEST(IFDSReachingDefinitionsFlowTest, ReturnFlowMapsReturnValueToCallResult) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto Out = Analysis.return_flow(
      IR.Call, IR.CallReturnSite, IR.Callee,
      ifds::DefinitionFact::definition(IR.Formal, IR.CalleeEntryInst),
      ifds::DefinitionFact::zero());

  EXPECT_TRUE(containsDefinition(Out, IR.Call, IR.CalleeEntryInst));
}

TEST(IFDSReachingDefinitionsFlowTest, CallToReturnKillsCalleeNonLocalFacts) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto Out = Analysis.call_to_return_flow(
      IR.Call, IR.CallReturnSite,
      ifds::DefinitionFact::definition(IR.Formal, IR.CalleeEntryInst));

  EXPECT_TRUE(Out.empty());
}

TEST(IFDSReachingDefinitionsFlowTest, CallToReturnKeepsCallerLocalFacts) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto InFact = ifds::DefinitionFact::definition(IR.Actual, IR.Call);
  auto Out = Analysis.call_to_return_flow(IR.Call, IR.CallReturnSite, InFact);

  EXPECT_EQ(Out.count(InFact), 1U);
}

TEST(IFDSReachingDefinitionsFlowTest, ExternalCallKillsGlobalsAndKeepsZero) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto GlobalFact = ifds::DefinitionFact::definition(IR.Global, IR.Call);
  auto GlobalOut =
      Analysis.call_to_return_flow(IR.ExtCall, IR.ExtCallReturnSite, GlobalFact);
  EXPECT_TRUE(GlobalOut.empty());

  auto ZeroOut = Analysis.call_to_return_flow(IR.ExtCall, IR.ExtCallReturnSite,
                                              ifds::DefinitionFact::zero());
  EXPECT_EQ(ZeroOut.count(ifds::DefinitionFact::zero()), 1U);
}
