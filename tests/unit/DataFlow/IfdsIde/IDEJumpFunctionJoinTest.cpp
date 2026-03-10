#include <gtest/gtest.h>
#include <Dataflow/IFDS/Solvers/IDESolver.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <set>

namespace ifds {
namespace {

struct SetValue {
  bool is_bottom = true;
  std::set<int> values;

  static SetValue bottom() { return SetValue{}; }
  static SetValue singleton(int v) {
    SetValue s;
    s.is_bottom = false;
    s.values.insert(v);
    return s;
  }
  bool operator==(const SetValue &o) const {
    return is_bottom == o.is_bottom && values == o.values;
  }
};

class JumpJoinProblem : public IDEProblem<const llvm::Value *, SetValue> {
public:
  using Fact = const llvm::Value *;
  using Value = SetValue;

  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction * /*stmt*/,
                      const llvm::Instruction * /*succ*/,
                      const Fact &fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase * /*call*/,
                    const llvm::Function * /*callee*/,
                    const Fact & /*fact*/) override {
    return {};
  }
  FactSet return_flow(const llvm::CallBase * /*call*/,
                      const llvm::Instruction * /*exit_inst*/,
                      const llvm::Instruction *return_site, const llvm::Function * /*callee*/,
                      const Fact & /*exit_fact*/,
                      const Fact & /*call_fact*/) override {
    (void)return_site;
    return {};
  }
  FactSet call_to_return_flow(const llvm::CallBase * /*call*/,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> /*callees*/,
                              const Fact &fact) override {
    (void)return_site;
    return {fact};
  }
  FactSet initial_facts(const llvm::Function * /*main*/) override {
    return {zero_fact()};
  }

  Value top_value() const override { return Value::bottom(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override {
    if (v1.is_bottom) {
      return v2;
    }
    if (v2.is_bottom) {
      return v1;
    }
    Value out;
    out.is_bottom = false;
    out.values = v1.values;
    out.values.insert(v2.values.begin(), v2.values.end());
    return out;
  }

  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const llvm::Instruction * /*succ*/,
                                    const Fact & /*src_fact*/,
                                    const Fact & /*tgt_fact*/) override {
    if (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
      if (bin->getName() == "then_add") {
        return [](const Value & /*in*/) { return Value::singleton(2); };
      }
      if (bin->getName() == "else_add") {
        return [](const Value & /*in*/) { return Value::singleton(4); };
      }
    }
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase * /*call*/,
                                  const llvm::Function * /*callee*/,
                                  const Fact & /*src_fact*/,
                                  const Fact & /*tgt_fact*/) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase * /*call*/,
                                    const llvm::Function * /*callee*/,
                                    const llvm::Instruction * /*exit_inst*/,
                                    const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
                                    const Fact & /*ret_fact*/) override {
    (void)return_site;
    return identity();
  }
  EdgeFunction call_to_return_edge_function(const llvm::CallBase * /*call*/,
                                            const llvm::Instruction *return_site,
                                            llvm::ArrayRef<const llvm::Function *> /*callees*/,
                                            const Fact & /*src_fact*/,
                                            const Fact & /*tgt_fact*/) override {
    (void)return_site;
    return identity();
  }
};

class EquivalentLoopProblem : public IDEProblem<const llvm::Value *, SetValue> {
public:
  using Fact = const llvm::Value *;
  using Value = SetValue;

  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction *, const llvm::Instruction *,
                      const Fact &fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const Fact &) override {
    return {};
  }
  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *, const llvm::Function *,
                      const Fact &, const Fact &) override {
    return {};
  }
  FactSet call_to_return_flow(const llvm::CallBase *,
                              const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *>,
                              const Fact &) override {
    return {};
  }
  FactSet initial_facts(const llvm::Function *) override { return {zero_fact()}; }

  Value top_value() const override { return Value::bottom(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override {
    if (v1.is_bottom) {
      return v2;
    }
    if (v2.is_bottom) {
      return v1;
    }
    Value out;
    out.is_bottom = false;
    out.values = v1.values;
    out.values.insert(v2.values.begin(), v2.values.end());
    return out;
  }

  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    if (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
      if (bin->getName() == "loop_add") {
        return [](const Value &) { return Value::singleton(7); };
      }
    }
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase *,
                                  const llvm::Function *, const Fact &,
                                  const Fact &) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase *,
                                    const llvm::Function *,
                                    const llvm::Instruction *,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction call_to_return_edge_function(const llvm::CallBase *,
                                            const llvm::Instruction *,
                                            llvm::ArrayRef<const llvm::Function *>,
                                            const Fact &, const Fact &) override {
    return identity();
  }
};

TEST(IDEJumpFunctionJoinTest, JoinsFunctionsFromDistinctPaths) {
  llvm::LLVMContext Ctx;
  auto M = std::make_unique<llvm::Module>("jump_join", Ctx);
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *I1 = llvm::Type::getInt1Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I1}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage, "main",
                                   M.get());

  auto *argIt = F->arg_begin();
  llvm::Value *Cond = &*argIt;

  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *ThenBB = llvm::BasicBlock::Create(Ctx, "then", F);
  auto *ElseBB = llvm::BasicBlock::Create(Ctx, "else", F);
  auto *MergeBB = llvm::BasicBlock::Create(Ctx, "merge", F);

  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(Cond, ThenBB, ElseBB);

  B.SetInsertPoint(ThenBB);
  auto *ThenSlot = B.CreateAlloca(I32, nullptr, "then_slot");
  B.CreateStore(llvm::ConstantInt::get(I32, 1), ThenSlot);
  auto *ThenLoad = B.CreateLoad(I32, ThenSlot, "then_ld");
  B.CreateAdd(ThenLoad, llvm::ConstantInt::get(I32, 1), "then_add");
  B.CreateBr(MergeBB);

  B.SetInsertPoint(ElseBB);
  auto *ElseSlot = B.CreateAlloca(I32, nullptr, "else_slot");
  B.CreateStore(llvm::ConstantInt::get(I32, 2), ElseSlot);
  auto *ElseLoad = B.CreateLoad(I32, ElseSlot, "else_ld");
  B.CreateAdd(ElseLoad, llvm::ConstantInt::get(I32, 2), "else_add");
  B.CreateBr(MergeBB);

  B.SetInsertPoint(MergeBB);
  auto *Ret = B.CreateRet(llvm::ConstantInt::get(I32, 0));

  JumpJoinProblem Problem;
  IDESolver<JumpJoinProblem> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, nullptr);
  EXPECT_FALSE(V.is_bottom);
  EXPECT_EQ(V.values.size(), 2u);
  EXPECT_TRUE(V.values.count(2) > 0);
  EXPECT_TRUE(V.values.count(4) > 0);
}

TEST(IDEJumpFunctionJoinTest, EquivalentLoopFunctionsReachFixpoint) {
  llvm::LLVMContext Ctx;
  auto M = std::make_unique<llvm::Module>("equivalent_loop", Ctx);
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *I1 = llvm::Type::getInt1Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I1}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage, "main",
                                   M.get());

  auto *Cond = &*F->arg_begin();
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Ctx, "loop", F);
  auto *Exit = llvm::BasicBlock::Create(Ctx, "exit", F);

  llvm::IRBuilder<> B(Entry);
  B.CreateBr(Loop);

  B.SetInsertPoint(Loop);
  auto *Slot = B.CreateAlloca(I32, nullptr, "slot");
  auto *Load = B.CreateLoad(I32, Slot, "loop_ld");
  auto *Add = B.CreateAdd(Load, llvm::ConstantInt::get(I32, 1), "loop_add");
  (void)Add;
  B.CreateCondBr(Cond, Loop, Exit);

  B.SetInsertPoint(Exit);
  auto *Ret = B.CreateRet(llvm::ConstantInt::get(I32, 0));

  EquivalentLoopProblem Problem;
  IDESolver<EquivalentLoopProblem> Solver(Problem);
  Solver.set_max_steps(50);
  Solver.solve(*M);

  EXPECT_FALSE(Solver.bound_reached());
  auto V = Solver.get_value_at(Ret, nullptr);
  EXPECT_FALSE(V.is_bottom);
  EXPECT_TRUE(V.values.count(7) > 0);
}

} // namespace
} // namespace ifds

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
