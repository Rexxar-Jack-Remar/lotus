#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <Dataflow/IFDS/Solvers/PathAwareIFDSSolver.h>
#include <gtest/gtest.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <set>

namespace ifds {
namespace {

class PathAwareSolverTest : public ::testing::Test {
protected:
  std::unique_ptr<llvm::LLVMContext> ctx = std::make_unique<llvm::LLVMContext>();

  std::unique_ptr<llvm::Module> createIdentityFlowModule() {
    auto m = std::make_unique<llvm::Module>("pathaware_identity", *ctx);
    auto *i32 = llvm::Type::getInt32Ty(*ctx);
    auto *mainTy = llvm::FunctionType::get(i32, {}, false);
    auto *sourceTy = llvm::FunctionType::get(i32, {}, false);
    auto *identityTy = llvm::FunctionType::get(i32, {i32}, false);
    auto *sinkTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), {i32}, false);

    auto *mainFn =
        llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage, "main", m.get());
    auto *sourceFn = llvm::Function::Create(sourceTy, llvm::Function::ExternalLinkage,
                                            "source", m.get());
    auto *identityFn = llvm::Function::Create(identityTy, llvm::Function::InternalLinkage,
                                              "identity", m.get());
    auto *sinkFn = llvm::Function::Create(sinkTy, llvm::Function::ExternalLinkage, "sink",
                                          m.get());

    auto *entry = llvm::BasicBlock::Create(*ctx, "entry", mainFn);
    llvm::IRBuilder<> b(entry);
    auto *tainted = b.CreateCall(sourceTy, sourceFn, {});
    auto *pass = b.CreateCall(identityTy, identityFn, {tainted});
    b.CreateCall(sinkTy, sinkFn, {pass});
    b.CreateRet(llvm::ConstantInt::get(i32, 0));

    auto *idEntry = llvm::BasicBlock::Create(*ctx, "entry", identityFn);
    llvm::IRBuilder<> ib(idEntry);
    ib.CreateRet(identityFn->getArg(0));

    return m;
  }
};

TEST_F(PathAwareSolverTest, ESGIsPopulatedAfterSolve) {
  auto m = createIdentityFlowModule();
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  solver.solve(*m);

  EXPECT_GT(solver.get_esg_node_count(), 0u);
  EXPECT_GT(solver.get_esg_edge_count(), 0u);
}

TEST_F(PathAwareSolverTest, RecordsSummaryEdges) {
  auto m = createIdentityFlowModule();
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  solver.solve(*m);

  bool has_summary = false;
  for (const auto &edge : solver.get_esg().get_all_edges()) {
    if (edge.kind == ESGEdgeKind::Summary) {
      has_summary = true;
      break;
    }
  }
  EXPECT_TRUE(has_summary);
}

TEST_F(PathAwareSolverTest, IFDSAndPathAwareParityAtSinkCall) {
  auto m = createIdentityFlowModule();
  TaintAnalysis taint1;
  taint1.add_source_function("source");
  taint1.add_sink_function("sink");

  TaintAnalysis taint2;
  taint2.add_source_function("source");
  taint2.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> ifds_solver(taint1);
  ifds_solver.solve(*m);

  PathAwareIFDSSolver<TaintAnalysis> path_solver(taint2);
  path_solver.solve(*m);

  const llvm::Instruction *sink_call = nullptr;
  auto *mainFn = m->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  for (const auto &bb : *mainFn) {
    for (const auto &inst : bb) {
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "sink") {
          sink_call = call;
        }
      }
    }
  }
  ASSERT_NE(sink_call, nullptr);

  auto baseline = ifds_solver.get_facts_at_entry(sink_call);
  auto pathaware = path_solver.get_facts_at_entry(sink_call);
  std::set<TaintFact> baseline_nonzero;
  for (const auto &fact : baseline) {
    if (!taint1.is_zero_fact(fact)) {
      baseline_nonzero.insert(fact);
    }
  }
  EXPECT_EQ(baseline_nonzero, pathaware);
}

} // namespace
} // namespace ifds

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
