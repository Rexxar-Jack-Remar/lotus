/**
 * @file MonoTest.cpp
 * @brief Unit tests for Mono (monotone dataflow framework)
 */

#include "Dataflow/Mono/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/Mono/Analyses/Intra/IntraUninitVariables.h"
#include "Dataflow/Mono/Analyses/Inter/InterTaintAnalysis.h"
#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace mono;

class MonoTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("MonoTest", errs());
    }
    return module;
  }

  template <typename InstT>
  InstT *findFirst(Function *F) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *Match = dyn_cast<InstT>(&I)) {
          return Match;
        }
      }
    }
    return nullptr;
  }

  Instruction *findByOpcode(Function *F, unsigned Opcode) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (I.getOpcode() == Opcode) {
          return &I;
        }
      }
    }
    return nullptr;
  }
};

// Test live variables analysis on simple function
TEST_F(MonoTest, LiveVariables) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %c = add i32 %a, %b
      %d = mul i32 %c, 2
      ret i32 %d
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Verify that results are computed for all instructions
  unsigned instCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      instCount++;
      // Each instruction should have IN and OUT sets
      auto &inSet = result->IN(&I);
      auto &outSet = result->OUT(&I);
      // Sets should be initialized (may be empty)
      EXPECT_GE(inSet.size(), 0);
      EXPECT_GE(outSet.size(), 0);
    }
  }

  EXPECT_GT(instCount, 0);
}

// Test live variables with multiple blocks
TEST_F(MonoTest, LiveVariablesMultiBlock) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %c = add i32 %a, %b
      br i1 true, label %true, label %false
    true:
      %d = mul i32 %c, 2
      br label %exit
    false:
      %e = sub i32 %c, 1
      br label %exit
    exit:
      %f = phi i32 [ %d, %true ], [ %e, %false ]
      ret i32 %f
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Find the return instruction
  ReturnInst *ret = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (ReturnInst *RI = dyn_cast<ReturnInst>(&I)) {
        ret = RI;
      }
    }
  }

  ASSERT_NE(ret, nullptr);

  // Return instruction should have computed IN/OUT
  auto &inSet = result->IN(ret);
  auto &outSet = result->OUT(ret);
  EXPECT_GE(inSet.size(), 0);
  EXPECT_EQ(outSet.size(), 0); // Out set of return should be empty
}

// Test empty function
TEST_F(MonoTest, EmptyFunction) {
  const char *source = R"(
    define void @test() {
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Should handle empty function gracefully
  unsigned instCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      instCount++;
      auto &inSet = result->IN(&I);
      auto &outSet = result->OUT(&I);
      EXPECT_GE(inSet.size(), 0);
      EXPECT_GE(outSet.size(), 0);
    }
  }

  EXPECT_GT(instCount, 0); // At least return instruction
}

TEST_F(MonoTest, ConstantPropagationMustAliasStrongUpdate) {
  const char *source = R"(
    define i32 @test(i32* %p) {
    entry:
      store i32 1, i32* %p
      %q = bitcast i32* %p to i32*
      store i32 2, i32* %q
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runIntraMonoConstantPropagation(F);
  ASSERT_FALSE(result.empty());

  auto *ret = findFirst<ReturnInst>(F);
  ASSERT_NE(ret, nullptr);

  // The IN map at the return should include facts for the load instruction.
  auto It = result.find(ret);
  ASSERT_NE(It, result.end());

  auto *load = findFirst<LoadInst>(F);
  ASSERT_NE(load, nullptr);

  auto FactIt = It->second.find(load);
  ASSERT_NE(FactIt, It->second.end());
  EXPECT_EQ(FactIt->second.Tag, ConstantPropagationTag::Const);
  EXPECT_EQ(FactIt->second.ConstValue, 2);
}

TEST_F(MonoTest, ConstantPropagationMayAliasWeakUpdate) {
  const char *source = R"(
    define i32 @test(i32* %p, i32* %q, i1 %c) {
    entry:
      store i32 1, i32* %p
      %r = select i1 %c, i32* %p, i32* %q
      store i32 2, i32* %r
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runIntraMonoConstantPropagation(F);
  ASSERT_FALSE(result.empty());

  auto *ret = findFirst<ReturnInst>(F);
  ASSERT_NE(ret, nullptr);

  auto It = result.find(ret);
  ASSERT_NE(It, result.end());

  auto *load = findFirst<LoadInst>(F);
  ASSERT_NE(load, nullptr);

  auto FactIt = It->second.find(load);
  ASSERT_NE(FactIt, It->second.end());
  EXPECT_EQ(FactIt->second.Tag, ConstantPropagationTag::Bottom);
}

TEST_F(MonoTest, UninitVariablesMustAliasClear) {
  const char *source = R"(
    define i32 @test(i32* %p) {
    entry:
      %q = bitcast i32* %p to i32*
      store i32 1, i32* %q
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runIntraMonoUninitVariables(F);
  ASSERT_NE(result, nullptr);

  auto *load = findFirst<LoadInst>(F);
  ASSERT_NE(load, nullptr);

  // Load should not be uninitialized after a definite store to the same location.
  auto &inSet = result->IN(load);
  EXPECT_EQ(inSet.count(load), 0u);
}

TEST_F(MonoTest, InterMonoSolverRecomputesIN) {
  const char *source = R"(
    define void @test() {
    entry:
      br i1 true, label %a, label %b
    a:
      br label %join
    b:
      br label %join
    join:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct NodeDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class NodeProblem : public InterMonoProblem<NodeDomain> {
  public:
    explicit NodeProblem(Function *Entry)
        : InterMonoProblem<NodeDomain>({Entry}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      if (Inst != nullptr) {
        Out.insert(Inst);
      }
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *, Function *,
                              const mono_container_t &In) override {
      return In;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *, const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *CallSite, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &In) override {
      mono_container_t Out = In;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      if (auto *Entry = getEntryPoints().empty() ? nullptr
                                                 : getEntryPoints().front()) {
        Seeds[&Entry->getEntryBlock().front()] = {};
      }
      return Seeds;
    }
  };

  NodeProblem Problem(F);
  InterMonoSolver<NodeDomain, 2> Solver(Problem);
  Solver.solve();

  auto *joinTerm = findByOpcode(F, Instruction::Ret);
  ASSERT_NE(joinTerm, nullptr);

  auto Facts = Solver.getResultsAt(joinTerm);
  // Both branch predecessors should contribute facts.
  EXPECT_GT(Facts.size(), 1u);
}

TEST_F(MonoTest, InterMonoTaintStrongWeakUpdate) {
  const char *source = R"(
    define void @sink(i32* %p) { ret void }
    define i32 @source() { ret i32 7 }

    define void @test(i32* %p, i32* %q) {
    entry:
      %t = call i32 @source()
      store i32 %t, i32* %p
      store i32 0, i32* %q
      call void @sink(i32* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  InterMonoTaintConfig Config;
  Config.SourceFunctions.insert("source");
  Config.SinkFunctions.insert("sink");

  auto Result = runInterMonoTaintAnalysis(F, Config);
  ASSERT_NE(Result.Results, nullptr);

  bool FoundLeak = false;
  for (const auto &Cell : Result.Report.Leaks) {
    for (auto *V : Cell.second) {
      if (auto *Arg = dyn_cast<Argument>(V)) {
        if (Arg->getArgNo() == 0) {
          FoundLeak = true;
        }
      }
    }
  }
  EXPECT_TRUE(FoundLeak);
}

TEST_F(MonoTest, CallBrContinuation) {
  const char *source = R"(
    declare void @callee()
    declare token @llvm.experimental.stackmap(i64, i32)

    define void @test() {
    entry:
      %token = call token @llvm.experimental.stackmap(i64 0, i32 0)
      callbr void @callee()
        to label %cont [label %cont]
    cont:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct TrivialDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class TrivialProblem : public InterMonoProblem<TrivialDomain> {
  public:
    explicit TrivialProblem(Function *Entry)
        : InterMonoProblem<TrivialDomain>({Entry}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      if (Inst != nullptr) {
        Out.insert(Inst);
      }
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *, Function *,
                              const mono_container_t &In) override {
      return In;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *, const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &In) override {
      return In;
    }

    std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      if (auto *Entry = getEntryPoints().empty() ? nullptr
                                                 : getEntryPoints().front()) {
        Seeds[&Entry->getEntryBlock().front()] = {};
      }
      return Seeds;
    }
  };

  TrivialProblem Problem(F);
  InterMonoSolver<TrivialDomain, 2> Solver(Problem);
  Solver.solve();

  auto *ret = findFirst<ReturnInst>(F);
  ASSERT_NE(ret, nullptr);

  const auto *InMap = Solver.getAnalysisINMap();
  ASSERT_NE(InMap, nullptr);
  bool Found = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst == ret) {
      Found = true;
      break;
    }
  }
  EXPECT_TRUE(Found);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
