#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "Verification/LoopInvariants/FunctionInvariantAnalysis.h"
#include "Verification/LoopInvariants/LoopInvariantAnalysis.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace lotus;

class LoopInvariantTest : public ::testing::Test {
protected:
  LLVMContext Context;
  std::unique_ptr<Module> M;

  void parseIR(const char *IR) {
    SMDiagnostic Err;
    M = parseAssemblyString(IR, Err, Context);
    ASSERT_TRUE(M != nullptr) << "Failed to parse IR";
  }
};

// Test 1: Simple ascending loop (original)
TEST_F(LoopInvariantTest, SimpleAscendingLoop) {
  const char *IR = R"(
    define i32 @simple_ascending_loop(i32 %n) {
    entry:
      br label %while.cond
    
    while.cond:
      %i = phi i32 [ 0, %entry ], [ %i.next, %while.body ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %while.body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %sum.next = add i32 %sum, %i
      %i.next = add i32 %i, 1
      br label %while.cond
    
    while.end:
      ret i32 %sum
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("simple_ascending_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for loop";
  if (Invs) {
    EXPECT_GT(Invs->size(), 0) << "Expected at least one invariant";

    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "Found invariant: " << Inv.DebugText << "\n";
    }
  }
}

// Test 2: Dual induction variables (original)
TEST_F(LoopInvariantTest, DualInductionVariables) {
  const char *IR = R"(
    define i32 @dual_induction_variables(i32 %n) {
    entry:
      br label %while.cond
    
    while.cond:
      %i = phi i32 [ 0, %entry ], [ %i.next, %while.body ]
      %j = phi i32 [ 0, %entry ], [ %j.next, %while.body ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %while.body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %tmp = add i32 %i, %j
      %sum.next = add i32 %sum, %tmp
      %i.next = add i32 %i, 1
      %j.next = add i32 %j, 2
      br label %while.cond
    
    while.end:
      ret i32 %sum
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("dual_induction_variables");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for loop";
  if (Invs) {
    EXPECT_GT(Invs->size(), 0) << "Expected at least one invariant";

    llvm::outs() << "Found " << Invs->size() << " invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.DebugText << "\n";
    }
  }
}

// Test 3: Decreasing loop (simplified)
TEST_F(LoopInvariantTest, DecreasingLoop) {
  // Use a simple loop that definitely terminates
  const char *IR = R"IR(
    define i32 @decreasing_loop(i32 %n) {
    entry:
      br label %while.cond
    
    while.cond:
      %i = phi i32 [ 100, %entry ], [ %i.next, %while.body ]
      %cmp = icmp sgt i32 %i, 0
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %i.next = sub i32 %i, 1
      br label %while.cond
    
    while.end:
      ret i32 0
    }
  )IR";

  parseIR(IR);

  Function *F = M->getFunction("decreasing_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for loop";
  if (Invs) {
    EXPECT_GT(Invs->size(), 0) << "Expected at least one invariant";
    llvm::outs() << "Found " << Invs->size()
                 << " decreasing loop invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.DebugText << "\n";
    }
  }
}

// Test 4: Pointer-based loop (terminator-like)
TEST_F(LoopInvariantTest, PointerLoop) {
  const char *IR = R"(
    define i8* @pointer_loop(i8* %arr, i32 %n) {
    entry:
      %ptr = bitcast i8* %arr to i8*
      br label %while.cond
    
    while.cond:
      %count = phi i32 [ 0, %entry ], [ %count.next, %while.body ]
      %cmp = icmp slt i32 %count, %n
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %elem = getelementptr i8, i8* %ptr, i32 %count
      %val = load i8, i8* %elem
      %count.next = add i32 %count, 1
      br label %while.cond
    
    while.end:
      ret i8* %ptr
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("pointer_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for pointer loop";
  if (Invs) {
    llvm::outs() << "Found " << Invs->size() << " pointer loop invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.DebugText << "\n";
    }
  }
}

// Test 5: Assignment-based loop with load
TEST_F(LoopInvariantTest, AssignmentBasedLoop) {
  const char *IR = R"(
    define i32 @assignment_loop(i32* %arr, i32 %n) {
    entry:
      br label %while.cond
    
    while.cond:
      %i = phi i32 [ 0, %entry ], [ %i.next, %while.body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %ptr = getelementptr i32, i32* %arr, i32 %i
      %val = load i32, i32* %ptr
      %result = add i32 %val, %i
      %i.next = add i32 %i, 1
      br label %while.cond
    
    while.end:
      ret i32 0
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("assignment_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for assignment loop";
  if (Invs) {
    llvm::outs() << "Found " << Invs->size()
                 << " assignment-based loop invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.DebugText << "\n";
    }
  }
}

// Test 6: Nested loops
TEST_F(LoopInvariantTest, NestedLoops) {
  const char *IR = R"(
    define i32 @nested_loop(i32 %n, i32 %m) {
    entry:
      br label %outer.cond
    
    outer.cond:
      %i = phi i32 [ 0, %entry ], [ %i.next, %outer.body ]
      %cmp.outer = icmp slt i32 %i, %n
      br i1 %cmp.outer, label %outer.body, label %outer.end
    
    outer.body:
      br label %inner.cond
    
    inner.cond:
      %j = phi i32 [ 0, %outer.body ], [ %j.next, %inner.body ]
      %cmp.inner = icmp slt i32 %j, %m
      br i1 %cmp.inner, label %inner.body, label %inner.end
    
    inner.body:
      %j.next = add i32 %j, 1
      br label %inner.cond
    
    inner.end:
      %i.next = add i32 %i, 1
      br label %outer.cond
    
    outer.end:
      ret i32 0
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("nested_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  auto Loops = LI.getLoopsInPreorder();
  ASSERT_GE(Loops.size(), 2u) << "Expected at least 2 loops (nested)";

  for (Loop *L : Loops) {
    const LoopInvariantSet *Invs = Result.getInvariants(L);
    EXPECT_TRUE(Invs != nullptr) << "No invariants found for a nested loop";
    if (Invs) {
      llvm::outs() << "Found " << Invs->size()
                   << " invariants for loop at depth " << L->getLoopDepth()
                   << "\n";
    }
  }
}

// Test 7: Function with return value invariant (addition)
TEST_F(LoopInvariantTest, FunctionReturnAddition) {
  const char *IR = R"(
    define i32 @return_sum(i32 %a, i32 %b) {
    entry:
      %result = add i32 %a, %b
      ret i32 %result
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("return_sum");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return FunctionInvariantAnalysis(); });

  auto &Result = FAM.getResult<FunctionInvariantAnalysis>(*F);

  const FunctionInvariantSet *Invs = Result.getInvariants(F);

  if (Invs) {
    llvm::outs() << "Found " << Invs->size() << " function invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.Description << ": "
                   << Inv.Formula.to_string() << "\n";
    }
  } else {
    llvm::outs() << "No function invariants found (may be expected)\n";
  }
}

// Test 8: Function with subtraction return
TEST_F(LoopInvariantTest, FunctionReturnSubtraction) {
  const char *IR = R"(
    define i32 @return_diff(i32 %a, i32 %b) {
    entry:
      %cmp = icmp sge i32 %a, %b
      br i1 %cmp, label %then, label %else
    
    then:
      %result = sub i32 %a, %b
      ret i32 %result
    
    else:
      ret i32 0
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("return_diff");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return FunctionInvariantAnalysis(); });

  auto &Result = FAM.getResult<FunctionInvariantAnalysis>(*F);

  const FunctionInvariantSet *Invs = Result.getInvariants(F);

  if (Invs) {
    llvm::outs() << "Found " << Invs->size()
                 << " function invariants for subtraction:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.Description << ": "
                   << Inv.Formula.to_string() << "\n";
    }
  } else {
    llvm::outs() << "No function invariants found for subtraction\n";
  }
}

// Test 9: Complex loop with multiple assignments
TEST_F(LoopInvariantTest, ComplexMultiAssignment) {
  const char *IR = R"(
    define i32 @complex_loop(i32 %n, i32 %m) {
    entry:
      br label %while.cond
    
    while.cond:
      %i = phi i32 [ 0, %entry ], [ %i.next, %while.body ]
      %x = phi i32 [ 5, %entry ], [ %x.next, %while.body ]
      %y = phi i32 [ 10, %entry ], [ %y.next, %while.body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %x.next = add i32 %x, %m
      %y.next = sub i32 %y, 1
      %i.next = add i32 %i, 1
      br label %while.cond
    
    while.end:
      ret i32 %x
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("complex_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for complex loop";
  if (Invs) {
    llvm::outs() << "Found " << Invs->size() << " complex loop invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.DebugText << "\n";
    }
  }
}

// Test 10: Loop with upper bound from parameter
TEST_F(LoopInvariantTest, UpperBoundLoop) {
  const char *IR = R"(
    define i32 @upper_bound_loop(i32 %limit) {
    entry:
      br label %while.cond
    
    while.cond:
      %i = phi i32 [ 0, %entry ], [ %i.next, %while.body ]
      %cmp = icmp sle i32 %i, %limit
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %i.next = add i32 %i, 1
      br label %while.cond
    
    while.end:
      ret i32 %i
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("upper_bound_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for upper bound loop";
  if (Invs) {
    bool foundBound = false;
    llvm::outs() << "Found " << Invs->size() << " upper bound invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.DebugText << "\n";
      if (Inv.DebugText.find("bound") != std::string::npos) {
        foundBound = true;
      }
    }
    EXPECT_TRUE(foundBound) << "Expected to find bound invariant";
  }
}

// Test 11: Function with constant return
TEST_F(LoopInvariantTest, FunctionConstantReturn) {
  const char *IR = R"(
    define i32 @constant_return(i32 %x) {
    entry:
      ret i32 42
    }
  )";

  parseIR(IR);

  Function *F = M->getFunction("constant_return");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return FunctionInvariantAnalysis(); });

  auto &Result = FAM.getResult<FunctionInvariantAnalysis>(*F);

  const FunctionInvariantSet *Invs = Result.getInvariants(F);

  llvm::outs() << "Function constant return test:\n";
  if (Invs) {
    llvm::outs() << "Found " << Invs->size() << " invariants\n";
  } else {
    llvm::outs() << "No invariants found (expected for constant return)\n";
  }
}

// Test 12: Flag-based loop detection (loop counter)
TEST_F(LoopInvariantTest, FlagBasedLoop) {
  const char *IR = R"(
    define i32 @flag_loop(i32 %n) {
    entry:
      %flag = call i32 @get_flag()
      br label %while.cond
    
    while.cond:
      %i = phi i32 [ 0, %entry ], [ %i.next, %while.body ]
      %f = phi i32 [ %flag, %entry ], [ %f.next, %while.body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %while.body, label %while.end
    
    while.body:
      %f.next = or i32 %f, 1
      %i.next = add i32 %i, 1
      br label %while.cond
    
    while.end:
      ret i32 %f
    }
    
    declare i32 @get_flag()
  )";

  parseIR(IR);

  Function *F = M->getFunction("flag_loop");
  ASSERT_TRUE(F != nullptr);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  FAM.registerPass([&] { return LoopInvariantAnalysis(); });

  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &Result = FAM.getResult<LoopInvariantAnalysis>(*F);

  ASSERT_FALSE(LI.empty()) << "No loops found in function";

  Loop *L = *LI.begin();
  const LoopInvariantSet *Invs = Result.getInvariants(L);

  EXPECT_TRUE(Invs != nullptr) << "No invariants found for flag loop";
  if (Invs) {
    llvm::outs() << "Found " << Invs->size()
                 << " flag-based loop invariants:\n";
    for (const auto &Inv : Invs->Invariants) {
      llvm::outs() << "  - " << Inv.DebugText << "\n";
    }
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
