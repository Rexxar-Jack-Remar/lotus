#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

#include <Dataflow/IFDS/IFDSFramework.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>

using namespace ifds;
using namespace llvm;

// ============================================================================
// IFDS Solver Tests - Function Summary Coverage
// ============================================================================

class IFDSSolverTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_unique<LLVMContext>();
    }
    
    std::unique_ptr<LLVMContext> context;
    
    // Helper: Create a module with source -> sink flow (normal flow)
    std::unique_ptr<Module> createLinearFlow() {
        auto module = std::make_unique<Module>("linear_flow", *context);
        auto* i32 = Type::getInt32Ty(*context);
        auto* mainTy = FunctionType::get(i32, {}, false);
        auto* sourceTy = FunctionType::get(i32, {}, false);
        auto* sinkTy = FunctionType::get(Type::getVoidTy(*context), {i32}, false);
        
        auto* main = Function::Create(mainTy, Function::ExternalLinkage, "main", module.get());
        auto* source = Function::Create(sourceTy, Function::ExternalLinkage, "source", module.get());
        auto* sink = Function::Create(sinkTy, Function::ExternalLinkage, "sink", module.get());
        
        auto* bb = BasicBlock::Create(*context, "entry", main);
        IRBuilder<> builder(bb);
        auto* tainted = builder.CreateCall(sourceTy, source, {});
        builder.CreateCall(sinkTy, sink, {tainted});
        builder.CreateRet(ConstantInt::get(i32, 0));
        
        return module;
    }
    
    // Helper: Create module with identity function (pass-through summary)
    std::unique_ptr<Module> createIdentityFlow() {
        auto module = std::make_unique<Module>("identity_flow", *context);
        auto* i32 = Type::getInt32Ty(*context);
        auto* mainTy = FunctionType::get(i32, {}, false);
        auto* sourceTy = FunctionType::get(i32, {}, false);
        auto* identityTy = FunctionType::get(i32, {i32}, false);
        auto* sinkTy = FunctionType::get(Type::getVoidTy(*context), {i32}, false);
        
        auto* main = Function::Create(mainTy, Function::ExternalLinkage, "main", module.get());
        auto* source = Function::Create(sourceTy, Function::ExternalLinkage, "source", module.get());
        auto* identity = Function::Create(identityTy, Function::InternalLinkage, "identity", module.get());
        auto* sink = Function::Create(sinkTy, Function::ExternalLinkage, "sink", module.get());
        
        // main: calls source -> identity -> sink
        auto* mainBB = BasicBlock::Create(*context, "entry", main);
        IRBuilder<> mainBuilder(mainBB);
        auto* tainted = mainBuilder.CreateCall(sourceTy, source, {});
        auto* passed = mainBuilder.CreateCall(identityTy, identity, {tainted});
        mainBuilder.CreateCall(sinkTy, sink, {passed});
        mainBuilder.CreateRet(ConstantInt::get(i32, 0));
        
        // identity: returns argument (pass-through)
        auto* idBB = BasicBlock::Create(*context, "entry", identity);
        IRBuilder<> idBuilder(idBB);
        idBuilder.CreateRet(identity->getArg(0));
        
        return module;
    }
    
    // Helper: Create module with summary reuse (multiple call sites)
    std::unique_ptr<Module> createReuseSummary() {
        auto module = std::make_unique<Module>("reuse_summary", *context);
        auto* i32 = Type::getInt32Ty(*context);
        auto* mainTy = FunctionType::get(i32, {}, false);
        auto* sourceTy = FunctionType::get(i32, {}, false);
        auto* processTy = FunctionType::get(i32, {i32}, false);
        auto* sinkTy = FunctionType::get(Type::getVoidTy(*context), {i32}, false);
        
        auto* main = Function::Create(mainTy, Function::ExternalLinkage, "main", module.get());
        auto* source = Function::Create(sourceTy, Function::ExternalLinkage, "source", module.get());
        auto* process = Function::Create(processTy, Function::InternalLinkage, "process", module.get());
        auto* sink = Function::Create(sinkTy, Function::ExternalLinkage, "sink", module.get());
        
        // main: calls process twice with different inputs
        auto* mainBB = BasicBlock::Create(*context, "entry", main);
        IRBuilder<> mainBuilder(mainBB);
        auto* tainted = mainBuilder.CreateCall(sourceTy, source, {});
        auto* result1 = mainBuilder.CreateCall(processTy, process, {tainted});
        auto* result2 = mainBuilder.CreateCall(processTy, process, {result1});
        mainBuilder.CreateCall(sinkTy, sink, {result2});
        mainBuilder.CreateRet(ConstantInt::get(i32, 0));
        
        // process: identity function
        auto* procBB = BasicBlock::Create(*context, "entry", process);
        IRBuilder<> procBuilder(procBB);
        procBuilder.CreateRet(process->getArg(0));
        
        return module;
    }
    
    // Helper: Create module with branching (control flow split)
    std::unique_ptr<Module> createBranchFlow() {
        auto module = std::make_unique<Module>("branch_flow", *context);
        auto* i32 = Type::getInt32Ty(*context);
        auto* i1 = Type::getInt1Ty(*context);
        auto* mainTy = FunctionType::get(i32, {}, false);
        auto* sourceTy = FunctionType::get(i32, {}, false);
        auto* sinkTy = FunctionType::get(Type::getVoidTy(*context), {i32}, false);
        
        auto* main = Function::Create(mainTy, Function::ExternalLinkage, "main", module.get());
        auto* source = Function::Create(sourceTy, Function::ExternalLinkage, "source", module.get());
        auto* sink = Function::Create(sinkTy, Function::ExternalLinkage, "sink", module.get());
        
        auto* entry = BasicBlock::Create(*context, "entry", main);
        auto* thenBB = BasicBlock::Create(*context, "then", main);
        auto* elseBB = BasicBlock::Create(*context, "else", main);
        auto* mergeBB = BasicBlock::Create(*context, "merge", main);
        
        IRBuilder<> entryBuilder(entry);
        auto* tainted = entryBuilder.CreateCall(sourceTy, source, {});
        auto* cond = entryBuilder.CreateICmpEQ(tainted, ConstantInt::get(i32, 0));
        entryBuilder.CreateCondBr(cond, thenBB, elseBB);
        
        IRBuilder<> thenBuilder(thenBB);
        thenBuilder.CreateBr(mergeBB);
        
        IRBuilder<> elseBuilder(elseBB);
        elseBuilder.CreateBr(mergeBB);
        
        IRBuilder<> mergeBuilder(mergeBB);
        auto* phi = mergeBuilder.CreatePHI(i32, 2);
        phi->addIncoming(tainted, thenBB);
        phi->addIncoming(tainted, elseBB);
        mergeBuilder.CreateCall(sinkTy, sink, {phi});
        mergeBuilder.CreateRet(ConstantInt::get(i32, 0));
        
        return module;
    }
};

// ============================================================================
// Test Cases - IFDS Summary Types
// ============================================================================

TEST_F(IFDSSolverTest, BasicSolverCreation) {
    // Sanity check: solver can be created and initialized
    TaintAnalysis analysis;
    IFDSSolver<TaintAnalysis> solver(analysis);
    EXPECT_TRUE(true);
}

TEST_F(IFDSSolverTest, NormalFlow) {
    // Tests: Normal intra-procedural flow (call-to-return edge bypassing callee)
    auto module = createLinearFlow();
    TaintAnalysis analysis;
    analysis.add_source_function("source");
    analysis.add_sink_function("sink");
    
    IFDSSolver<TaintAnalysis> solver(analysis);
    solver.solve(*module);
    
    // Verify taint reaches sink from source
    auto* main = module->getFunction("main");
    ASSERT_NE(main, nullptr);
    bool foundTaint = false;
    for (auto& bb : *main) {
        for (auto& inst : bb) {
            if (auto* call = dyn_cast<CallInst>(&inst)) {
                if (call->getCalledFunction() && call->getCalledFunction()->getName() == "sink") {
                    auto facts = solver.get_facts_at_entry(&inst);
                    foundTaint = !facts.empty();
                }
            }
        }
    }
    EXPECT_TRUE(foundTaint) << "Taint should flow from source to sink";
}

TEST_F(IFDSSolverTest, CallReturnSummary) {
    // Tests: Call flow -> Return flow (summary edge through identity function)
    auto module = createIdentityFlow();
    TaintAnalysis analysis;
    analysis.add_source_function("source");
    analysis.add_sink_function("sink");
    
    IFDSSolver<TaintAnalysis> solver(analysis);
    solver.solve(*module);
    
    // Verify summary edge was created for identity function
    std::vector<SummaryEdge<TaintFact>> summaries;
    solver.get_summary_edges(summaries);
    EXPECT_GT(summaries.size(), 0) << "Should create summary edges";
}

TEST_F(IFDSSolverTest, SummaryReuse) {
    // Tests: Same function called multiple times reuses computed summary
    auto module = createReuseSummary();
    TaintAnalysis analysis;
    analysis.add_source_function("source");
    analysis.add_sink_function("sink");
    
    IFDSSolver<TaintAnalysis> solver(analysis);
    solver.solve(*module);
    
    // Count path edges vs summary edges - summaries reduce redundant computation
    std::vector<PathEdge<TaintFact>> paths;
    std::vector<SummaryEdge<TaintFact>> summaries;
    solver.get_path_edges(paths);
    solver.get_summary_edges(summaries);
    
    EXPECT_GT(summaries.size(), 0) << "Should reuse summaries for repeated calls";
    EXPECT_LT(summaries.size(), paths.size()) << "Summaries should be fewer than paths";
}

TEST_F(IFDSSolverTest, BranchMerge) {
    // Tests: Flow-sensitive merge at phi nodes (branch convergence)
    auto module = createBranchFlow();
    TaintAnalysis analysis;
    analysis.add_source_function("source");
    analysis.add_sink_function("sink");
    
    IFDSSolver<TaintAnalysis> solver(analysis);
    solver.solve(*module);
    
    // Verify taint propagates through both branches and merges
    auto* main = module->getFunction("main");
    ASSERT_NE(main, nullptr);
    int phiCount = 0, sinkCallCount = 0;
    for (auto& bb : *main) {
        for (auto& inst : bb) {
            if (isa<PHINode>(&inst)) {
                auto facts = solver.get_facts_at_exit(&inst);
                if (!facts.empty()) phiCount++;
            }
            if (auto* call = dyn_cast<CallInst>(&inst)) {
                if (call->getCalledFunction() && call->getCalledFunction()->getName() == "sink") {
                    if (!solver.get_facts_at_entry(&inst).empty()) sinkCallCount++;
                }
            }
        }
    }
    EXPECT_GT(phiCount, 0) << "Taint should reach phi node";
    EXPECT_GT(sinkCallCount, 0) << "Taint should reach sink after merge";
}

// ============================================================================
// Main function for running tests
// ============================================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

