#include <Dataflow/IFDS/Clients/IDEExtendedTaintAnalysis.h>
#include <Dataflow/IFDS/Clients/IDEFeatureTaintAnalysis.h>
#include <Dataflow/IFDS/Clients/IDEGeneralizedLCA.h>
#include <Dataflow/IFDS/Clients/IDEInstInteractionAnalysis.h>
#include <Dataflow/IFDS/Clients/IDESecureHeapPropagation.h>
#include <Dataflow/IFDS/Solvers/IDESolver.h>
#include <gtest/gtest.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

namespace ifds {
namespace {

class IDENewAnalysesTest : public ::testing::Test {
protected:
  void SetUp() override { Ctx = std::make_unique<llvm::LLVMContext>(); }
  std::unique_ptr<llvm::LLVMContext> Ctx;
};

TEST_F(IDENewAnalysesTest, ExtendedTaintMarksSourceCallResultTainted) {
  auto M = std::make_unique<llvm::Module>("ide_ext_taint", *Ctx);
  auto *I8Ptr = llvm::Type::getInt8PtrTy(*Ctx);
  auto *MainTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), {}, false);
  auto *RecvTy = llvm::FunctionType::get(I8Ptr, {}, false);
  auto *Recv =
      llvm::Function::Create(RecvTy, llvm::Function::ExternalLinkage, "recv", M.get());
  auto *Main =
      llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", M.get());

  auto *Entry = llvm::BasicBlock::Create(*Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);
  auto *CallRecv = B.CreateCall(RecvTy, Recv, {}, "recv_val");
  B.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 0));

  IDEExtendedTaintAnalysis Problem;
  auto SF = Problem.summary_flow(CallRecv, Recv, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallRecv) > 0);

  auto EF = Problem.summary_edge_function(CallRecv, Problem.zero_fact(), CallRecv);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, ExtendedTaintValue::Tainted);
}

TEST_F(IDENewAnalysesTest, FeatureTaintAssignsSourceFeatureBit) {
  auto M = std::make_unique<llvm::Module>("ide_feature_taint", *Ctx);
  auto *I8Ptr = llvm::Type::getInt8PtrTy(*Ctx);
  auto *MainTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), {}, false);
  auto *RecvTy = llvm::FunctionType::get(I8Ptr, {}, false);
  auto *Recv =
      llvm::Function::Create(RecvTy, llvm::Function::ExternalLinkage, "recv", M.get());
  auto *Main =
      llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", M.get());

  auto *Entry = llvm::BasicBlock::Create(*Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);
  auto *CallRecv = B.CreateCall(RecvTy, Recv, {}, "recv_val");
  B.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 0));

  IDEFeatureTaintAnalysis Problem;
  auto SF = Problem.summary_flow(CallRecv, Recv, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallRecv) > 0);

  auto EF = Problem.summary_edge_function(CallRecv, Problem.zero_fact(), CallRecv);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, FeatureTaintValue::Features);
  EXPECT_NE(V.mask & (1ull << 0), 0ull);
}

TEST_F(IDENewAnalysesTest, SecureHeapMarksAllocatorResultAllocated) {
  auto M = std::make_unique<llvm::Module>("ide_secure_heap", *Ctx);
  auto *I8Ptr = llvm::Type::getInt8PtrTy(*Ctx);
  auto *MainTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), {}, false);
  auto *MallocTy =
      llvm::FunctionType::get(I8Ptr, {llvm::Type::getInt64Ty(*Ctx)}, false);
  auto *Malloc = llvm::Function::Create(MallocTy, llvm::Function::ExternalLinkage,
                                        "malloc", M.get());
  auto *Main =
      llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", M.get());

  auto *Entry = llvm::BasicBlock::Create(*Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);
  auto *CallMalloc = B.CreateCall(
      MallocTy, Malloc,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 16)}, "buf");
  B.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 0));

  IDESecureHeapPropagation Problem;
  auto SF = Problem.summary_flow(CallMalloc, Malloc, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallMalloc) > 0);

  auto EF =
      Problem.summary_edge_function(CallMalloc, Problem.zero_fact(), CallMalloc);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, SecureHeapValue::Allocated);
}

TEST_F(IDENewAnalysesTest, InstInteractionMarksLoadAsRead) {
  auto M = std::make_unique<llvm::Module>("ide_inst_interaction", *Ctx);
  auto *I32 = llvm::Type::getInt32Ty(*Ctx);
  auto *I32Ptr = llvm::Type::getInt32PtrTy(*Ctx);
  auto *MainTy = llvm::FunctionType::get(I32, {I32Ptr}, false);
  auto *Main =
      llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", M.get());

  auto argIt = Main->arg_begin();
  llvm::Value *PtrArg = &*argIt;

  auto *Entry = llvm::BasicBlock::Create(*Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);
  auto *Load = B.CreateLoad(I32, PtrArg, "loaded");
  auto *Ret = B.CreateRet(Load);

  IDEInstInteractionAnalysis Problem;
  IDESolver<IDEInstInteractionAnalysis> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Load);
  EXPECT_TRUE(V.kind == InstInteractionValue::Read ||
              V.kind == InstInteractionValue::ReadWrite);
}

TEST_F(IDENewAnalysesTest, GeneralizedLCAComputesConstantSet) {
  auto M = std::make_unique<llvm::Module>("ide_glca", *Ctx);
  auto *I32 = llvm::Type::getInt32Ty(*Ctx);
  auto *MainTy = llvm::FunctionType::get(I32, {}, false);
  auto *Main =
      llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", M.get());

  auto *Entry = llvm::BasicBlock::Create(*Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);
  auto *C3 = llvm::ConstantInt::get(I32, 3);
  auto *C4 = llvm::ConstantInt::get(I32, 4);
  auto *Add = llvm::BinaryOperator::CreateAdd(C3, C4, "sum", Entry);
  auto *Ret = B.CreateRet(Add);

  IDEGeneralizedLCA Problem;
  IDESolver<IDEGeneralizedLCA> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Add);
  EXPECT_EQ(V.kind, GLCAValue::ConstantSet);
  EXPECT_EQ(V.constants.size(), 1u);
  EXPECT_TRUE(V.constants.count(7) > 0);
}

} // namespace
} // namespace ifds

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
