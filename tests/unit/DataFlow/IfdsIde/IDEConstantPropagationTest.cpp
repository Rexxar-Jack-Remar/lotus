#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IDEConstantPropagation.h>
#include <Dataflow/IFDS/Solvers/IDESolver.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

namespace ifds {
namespace {

class IDEConstantPropagationTest : public ::testing::Test {
protected:
  void SetUp() override { Ctx = std::make_unique<llvm::LLVMContext>(); }

  std::unique_ptr<llvm::LLVMContext> Ctx;
};

TEST_F(IDEConstantPropagationTest, ComputeConstFromTwoConsts) {
  auto M = std::make_unique<llvm::Module>("lcp_two_consts", *Ctx);
  auto* I32 = llvm::Type::getInt32Ty(*Ctx);
  auto* MainTy = llvm::FunctionType::get(I32, {}, false);
  auto* Main =
      llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", M.get());

  auto* Entry = llvm::BasicBlock::Create(*Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);

  // Avoid constant-folding by creating the constants via memory.
  auto* A = B.CreateAlloca(I32, nullptr, "a");
  auto* BVar = B.CreateAlloca(I32, nullptr, "b");
  B.CreateStore(llvm::ConstantInt::get(I32, 1), A);
  B.CreateStore(llvm::ConstantInt::get(I32, 2), BVar);
  auto* L1 = B.CreateLoad(I32, A, "l1");
  auto* L2 = B.CreateLoad(I32, BVar, "l2");
  auto* Add = B.CreateAdd(L1, L2, "sum"); // Instruction*
  auto* Ret = B.CreateRet(Add);

  IDEConstantPropagation Problem;
  IDESolver<IDEConstantPropagation> Solver(Problem);
  Solver.solve(*M);

  // Values for newly-created facts are typically available at successor nodes.
  auto V = Solver.get_value_at(Ret, llvm::cast<llvm::Instruction>(Add));
  EXPECT_EQ(V.kind, LCPValue::Const);
  EXPECT_EQ(V.value, 3);
}

TEST_F(IDEConstantPropagationTest, PropagateThroughStoreLoadAndBinop) {
  auto M = std::make_unique<llvm::Module>("lcp_store_load", *Ctx);
  auto* I32 = llvm::Type::getInt32Ty(*Ctx);
  auto* MainTy = llvm::FunctionType::get(I32, {}, false);
  auto* Main =
      llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", M.get());

  auto* Entry = llvm::BasicBlock::Create(*Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);

  auto* X = B.CreateAlloca(I32, nullptr, "x");
  B.CreateStore(llvm::ConstantInt::get(I32, 5), X);
  auto* LoadX = B.CreateLoad(I32, X, "lx");
  auto* Add = B.CreateAdd(LoadX, llvm::ConstantInt::get(I32, 2), "plus2");
  auto* Ret = B.CreateRet(Add);

  IDEConstantPropagation Problem;
  IDESolver<IDEConstantPropagation> Solver(Problem);
  Solver.solve(*M);

  // Check load value at return (successor of all computations).
  auto LoadVal = Solver.get_value_at(Ret, LoadX);
  EXPECT_EQ(LoadVal.kind, LCPValue::Const);
  EXPECT_EQ(LoadVal.value, 5);

  // Check add result value at return.
  auto AddVal = Solver.get_value_at(Ret, Add);
  EXPECT_EQ(AddVal.kind, LCPValue::Const);
  EXPECT_EQ(AddVal.value, 7);
}

} // namespace
} // namespace ifds

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

