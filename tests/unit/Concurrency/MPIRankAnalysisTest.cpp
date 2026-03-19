#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace MPI;

static const Instruction *findInstructionByName(const Function &func,
                                                StringRef name) {
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

class MPIRankAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("MPIRankAnalysisTest", errs());
    }
    return module;
  }
};

TEST_F(MPIRankAnalysisTest, CommRankLoadsAreTrackedAsSymbolic) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      ret i32 %loaded
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *loaded =
      findInstructionByName(*module->getFunction("main"), "loaded");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(analysis.getRankExpr(loaded).kind, RankExpr::Symbolic);
}

TEST_F(MPIRankAnalysisTest, EqualityBranchRefinesTrueSuccessor) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_zero = icmp eq i32 %loaded, 0
      br i1 %is_zero, label %then, label %else

    then:
      %on_root = add i32 1, 2
      ret i32 %on_root

    else:
      %other = add i32 3, 4
      ret i32 %other
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *on_root =
      findInstructionByName(*module->getFunction("main"), "on_root");
  ASSERT_NE(on_root, nullptr);
  EXPECT_EQ(analysis.getRankAtInstruction(on_root).kind, RankExpr::Concrete);
  EXPECT_EQ(analysis.getRankAtInstruction(on_root).concrete_value, 0);
}

TEST_F(MPIRankAnalysisTest, OrderedInequalityRefinesRange) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %lt_two = icmp ult i32 %loaded, 2
      br i1 %lt_two, label %then, label %else

    then:
      %low_rank = add i32 1, 2
      ret i32 %low_rank

    else:
      %high_rank = add i32 3, 4
      ret i32 %high_rank
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *low_rank =
      findInstructionByName(*module->getFunction("main"), "low_rank");
  ASSERT_NE(low_rank, nullptr);
  RankExpr expr = analysis.getRankAtInstruction(low_rank);
  EXPECT_EQ(expr.kind, RankExpr::Range);
  EXPECT_EQ(expr.range_min, 0);
  EXPECT_EQ(expr.range_max, 1);
}

TEST_F(MPIRankAnalysisTest, CommSizeDerivedBoundRefinesRankRange) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Comm_size(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      %size = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      call i32 @MPI_Comm_size(i8* %comm, i32* %size)
      %loaded_rank = load i32, i32* %rank, align 4
      %loaded_size = load i32, i32* %size, align 4
      %limit = sub i32 %loaded_size, 1
      %in_range = icmp ult i32 %loaded_rank, %limit
      br i1 %in_range, label %then, label %else

    then:
      %before_last = add i32 1, 2
      ret i32 %before_last

    else:
      %last_or_after = add i32 3, 4
      ret i32 %last_or_after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *before_last =
      findInstructionByName(*module->getFunction("main"), "before_last");
  ASSERT_NE(before_last, nullptr);
  RankExpr expr = analysis.getRankAtInstruction(before_last);
  EXPECT_EQ(expr.kind, RankExpr::Range);
  EXPECT_EQ(expr.range_min, 0);
  EXPECT_EQ(expr.range_max, MPIRankAnalysis::defaultCommSizeUpperBound() - 2);
}

TEST_F(MPIRankAnalysisTest, WrapperPropagatesRankInformationToCaller) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define void @read_rank(i8* %comm, i32* %out) {
    entry:
      call i32 @MPI_Comm_rank(i8* %comm, i32* %out)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call void @read_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_zero = icmp eq i32 %loaded, 0
      br i1 %is_zero, label %then, label %else

    then:
      %on_root = add i32 1, 2
      ret i32 %on_root

    else:
      %other = add i32 3, 4
      ret i32 %other
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *on_root =
      findInstructionByName(*module->getFunction("main"), "on_root");
  ASSERT_NE(on_root, nullptr);
  EXPECT_EQ(analysis.getRankAtInstruction(on_root).kind, RankExpr::Concrete);
  EXPECT_EQ(analysis.getRankAtInstruction(on_root).concrete_value, 0);
}

TEST_F(MPIRankAnalysisTest,
       InequalityBranchProducesExcludedParticipantPredicate) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %not_root = icmp ne i32 %loaded, 0
      br i1 %not_root, label %then, label %else

    then:
      %non_root = add i32 1, 2
      ret i32 %non_root

    else:
      %root = add i32 3, 4
      ret i32 %root
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *non_root =
      findInstructionByName(*module->getFunction("main"), "non_root");
  ASSERT_NE(non_root, nullptr);

  MPIRankPredicate predicate = analysis.getRankPredicateAtInstruction(non_root);
  EXPECT_FALSE(predicate.unknown);
  EXPECT_TRUE(predicate.universal);
  EXPECT_EQ(predicate.excluded_ranks.count(0), 1u);
  EXPECT_NE(analysis.getPredicateClassAtInstruction(non_root), 0u);
  EXPECT_NE(analysis.getParticipantClassAtInstruction(non_root), 0u);
}

TEST_F(MPIRankAnalysisTest, RankPredicatesDoNotMergeAcrossCommunicators) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm0, i8* %comm1, i1 %pick_left) {
    entry:
      %rank0 = alloca i32, align 4
      %rank1 = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm0, i32* %rank0)
      call i32 @MPI_Comm_rank(i8* %comm1, i32* %rank1)
      %lhs = load i32, i32* %rank0, align 4
      %rhs = load i32, i32* %rank1, align 4
      br i1 %pick_left, label %left, label %right

    left:
      br label %join

    right:
      br label %join

    join:
      %merged = phi i32 [ %lhs, %left ], [ %rhs, %right ]
      ret i32 %merged
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *merged =
      findInstructionByName(*module->getFunction("main"), "merged");
  ASSERT_NE(merged, nullptr);
  MPIRankPredicate predicate = analysis.getRankPredicate(merged);
  EXPECT_TRUE(predicate.unknown);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
