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
  struct FlowNodes {
    const llvm::CallBase *source_call = nullptr;
    const llvm::CallBase *identity_call = nullptr;
    const llvm::CallBase *sink_call = nullptr;
    const llvm::Instruction *identity_entry = nullptr;
    const llvm::ReturnInst *identity_return = nullptr;
  };

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

  std::unique_ptr<llvm::Module> createMultiReturnIdentityModule() {
    auto m = std::make_unique<llvm::Module>("pathaware_multireturn", *ctx);
    auto *i32 = llvm::Type::getInt32Ty(*ctx);
    auto *mainTy = llvm::FunctionType::get(i32, {}, false);
    auto *sourceTy = llvm::FunctionType::get(i32, {}, false);
    auto *identityTy = llvm::FunctionType::get(i32, {i32}, false);
    auto *sinkTy =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), {i32}, false);

    auto *mainFn = llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage,
                                          "main", m.get());
    auto *sourceFn = llvm::Function::Create(sourceTy, llvm::Function::ExternalLinkage,
                                            "source", m.get());
    auto *identityFn =
        llvm::Function::Create(identityTy, llvm::Function::InternalLinkage,
                               "identity", m.get());
    auto *sinkFn = llvm::Function::Create(sinkTy, llvm::Function::ExternalLinkage,
                                          "sink", m.get());

    auto *entry = llvm::BasicBlock::Create(*ctx, "entry", mainFn);
    llvm::IRBuilder<> b(entry);
    auto *tainted = b.CreateCall(sourceTy, sourceFn, {});
    auto *pass = b.CreateCall(identityTy, identityFn, {tainted});
    b.CreateCall(sinkTy, sinkFn, {pass});
    b.CreateRet(llvm::ConstantInt::get(i32, 0));

    auto *idEntry = llvm::BasicBlock::Create(*ctx, "entry", identityFn);
    auto *idThen = llvm::BasicBlock::Create(*ctx, "then", identityFn);
    auto *idElse = llvm::BasicBlock::Create(*ctx, "else", identityFn);
    llvm::IRBuilder<> ib(idEntry);
    auto *cond = ib.CreateICmpEQ(identityFn->getArg(0), llvm::ConstantInt::get(i32, 0));
    ib.CreateCondBr(cond, idThen, idElse);
    llvm::IRBuilder<> thenBuilder(idThen);
    thenBuilder.CreateRet(identityFn->getArg(0));
    llvm::IRBuilder<> elseBuilder(idElse);
    elseBuilder.CreateRet(identityFn->getArg(0));

    return m;
  }

  FlowNodes collectFlowNodes(const llvm::Module &m) {
    FlowNodes nodes;
    auto *mainFn = m.getFunction("main");
    auto *identityFn = m.getFunction("identity");
    EXPECT_NE(mainFn, nullptr);
    EXPECT_NE(identityFn, nullptr);

    if (identityFn && !identityFn->empty()) {
      nodes.identity_entry = &identityFn->getEntryBlock().front();
      nodes.identity_return =
          llvm::dyn_cast<llvm::ReturnInst>(identityFn->getEntryBlock().getTerminator());
    }

    for (const auto &bb : *mainFn) {
      for (const auto &inst : bb) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call || !call->getCalledFunction()) {
          continue;
        }
        auto name = call->getCalledFunction()->getName();
        if (name == "source") {
          nodes.source_call = call;
        } else if (name == "identity") {
          nodes.identity_call = call;
        } else if (name == "sink") {
          nodes.sink_call = call;
        }
      }
    }
    return nodes;
  }
};

TEST_F(PathAwareSolverTest, RecordsImmediateInterproceduralEdges) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  solver.solve(*m);

  bool has_call = false;
  bool has_return = false;
  bool has_summary = false;
  bool has_long_range_source_to_sink = false;
  for (const auto &edge : solver.get_esg().get_all_edges()) {
    if (edge.kind == ESGEdgeKind::Call &&
        edge.source.instruction == nodes.identity_call &&
        edge.target.instruction == nodes.identity_entry) {
      has_call = true;
    }
    if (edge.kind == ESGEdgeKind::Return &&
        edge.source.instruction == nodes.identity_return &&
        edge.target.instruction == nodes.sink_call) {
      has_return = true;
    }
    if (edge.kind == ESGEdgeKind::Summary &&
        edge.source.instruction == nodes.identity_call &&
        edge.target.instruction == nodes.sink_call) {
      has_summary = true;
    }
    if (edge.source.instruction == nodes.source_call &&
        edge.target.instruction == nodes.sink_call) {
      has_long_range_source_to_sink = true;
    }
  }

  EXPECT_TRUE(has_call);
  EXPECT_TRUE(has_return);
  EXPECT_TRUE(has_summary);
  EXPECT_FALSE(has_long_range_source_to_sink);
}

TEST_F(PathAwareSolverTest, FindPathsReturnsRealInterproceduralPath) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> path_solver(taint);
  path_solver.solve(*m);

  bool checked_path = false;
  for (const auto &call_edge : path_solver.get_esg().get_all_edges()) {
    if (call_edge.kind != ESGEdgeKind::Call ||
        call_edge.source.instruction != nodes.identity_call ||
        call_edge.target.instruction != nodes.identity_entry) {
      continue;
    }
    auto paths = path_solver.find_paths(
        call_edge.source.instruction, call_edge.source.fact, nodes.sink_call,
        call_edge.source.fact, 4, 8);
    if (paths.empty()) {
      continue;
    }
    checked_path = true;
    bool saw_call = false;
    bool saw_return = false;
    for (const auto &path : paths) {
      for (const auto &edge : path) {
        saw_call |= edge.kind == ESGEdgeKind::Call;
        saw_return |= edge.kind == ESGEdgeKind::Return;
      }
    }
    EXPECT_TRUE(saw_call);
    EXPECT_TRUE(saw_return);
    break;
  }

  EXPECT_TRUE(checked_path);
}

TEST_F(PathAwareSolverTest, IFDSAndPathAwareParityAtSinkCall) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
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

  auto baseline = ifds_solver.get_facts_at_entry(nodes.sink_call);
  auto pathaware = path_solver.get_facts_at_entry(nodes.sink_call);
  std::set<TaintFact> baseline_nonzero;
  for (const auto &fact : baseline) {
    if (!taint1.is_zero_fact(fact)) {
      baseline_nonzero.insert(fact);
    }
  }
  EXPECT_EQ(baseline_nonzero, pathaware);
}

TEST_F(PathAwareSolverTest, MultiReturnSummaryReuseUsesRealExitNode) {
  auto m = createMultiReturnIdentityModule();
  auto nodes = collectFlowNodes(*m);
  auto *identityFn = m->getFunction("identity");
  ASSERT_NE(identityFn, nullptr);

  const llvm::ReturnInst *thenRet = nullptr;
  const llvm::ReturnInst *elseRet = nullptr;
  for (const auto &bb : *identityFn) {
    if (bb.getName() == "then") {
      thenRet = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    } else if (bb.getName() == "else") {
      elseRet = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    }
  }
  ASSERT_NE(thenRet, nullptr);
  ASSERT_NE(elseRet, nullptr);

  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  solver.solve(*m);

  bool has_precise_return = false;
  bool has_fabricated_return = false;
  for (const auto &edge : solver.get_esg().get_all_edges()) {
    if (edge.kind != ESGEdgeKind::Return ||
        edge.target.instruction != nodes.sink_call) {
      continue;
    }
    if (edge.source.instruction == thenRet || edge.source.instruction == elseRet) {
      has_precise_return = true;
    }
    if (edge.source.instruction == &identityFn->back().back() &&
        edge.source.instruction != thenRet && edge.source.instruction != elseRet) {
      has_fabricated_return = true;
    }
  }

  EXPECT_TRUE(has_precise_return);
  EXPECT_FALSE(has_fabricated_return);
}

TEST_F(PathAwareSolverTest, FactsRemainQueryableWhenComputeValuesDisabled) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  auto config = solver.get_solver_config();
  config.set_compute_values(false);
  solver.set_solver_config(config);
  solver.solve(*m);

  auto facts = solver.get_facts_at_entry(nodes.sink_call);
  EXPECT_FALSE(facts.empty());
}

TEST_F(PathAwareSolverTest, SolvePreservesRecordEdgesConfig) {
  auto m = createIdentityFlowModule();
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  auto config = solver.get_solver_config();
  config.set_record_edges(false);
  solver.set_solver_config(config);

  solver.solve(*m);

  EXPECT_FALSE(solver.get_solver_config().record_edges());
}

} // namespace
} // namespace ifds

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
