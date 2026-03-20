#include <gtest/gtest.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/SourceMgr.h>

#define private public
#define protected public
#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#undef protected
#undef private

using namespace llvm;

namespace {

std::unique_ptr<Module> parseAssembly(LLVMContext &Ctx, const char *IR) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("LotusAATest", errs());
  return M;
}

LotusAA *runLotusAA(Module &M) {
  auto *PM = new legacy::PassManager();
  auto *Pass = new LotusAA();
  PM->add(Pass);
  PM->run(M);
  return Pass;
}

void computeAllFunctionCgs(Module &M, LotusAA &Pass) {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (auto *PTG = Pass.getPtGraph(&F))
      PTG->computeCG();
  }
}

std::vector<CallBase *> getIndirectCalls(Function &F) {
  std::vector<CallBase *> Calls;
  for (Instruction &I : instructions(F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (CB->isIndirectCall())
        Calls.push_back(CB);
    }
  }
  return Calls;
}

struct LotusConfigScope {
  int inline_depth = IntraLotusAAConfig::lotus_restrict_inline_depth;
  int summary_ap_depth = IntraLotusAAConfig::lotus_restrict_summary_ap_depth;
  int inline_size = IntraLotusAAConfig::lotus_restrict_inline_size;
  int ap_level = IntraLotusAAConfig::lotus_restrict_ap_level;

  ~LotusConfigScope() {
    IntraLotusAAConfig::lotus_restrict_inline_depth = inline_depth;
    IntraLotusAAConfig::lotus_restrict_summary_ap_depth = summary_ap_depth;
    IntraLotusAAConfig::lotus_restrict_inline_size = inline_size;
    IntraLotusAAConfig::lotus_restrict_ap_level = ap_level;
  }
};

} // namespace

TEST(LotusAA, PhiMergeResolvesBothIndirectTargets) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %sub = sub i32 %x, 1
      ret i32 %sub
    }

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @bar, %else ]
      %res = call i32 %fp(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 2u);
  EXPECT_TRUE(Targets.count(M->getFunction("foo")));
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, StructFieldGepKeepsNonZeroOffsetPrecision) {
  const char *IR = R"(
    %struct.S = type { i32 (i32)*, i32 (i32)* }

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %add = add i32 %x, 42
      ret i32 %add
    }

    define i32 @main() {
    entry:
      %s = alloca %struct.S
      %f0 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 1
      store i32 (i32)* @foo, i32 (i32)** %f0
      store i32 (i32)* @bar, i32 (i32)** %f1
      %loaded = load i32 (i32)*, i32 (i32)** %f1
      %res = call i32 %loaded(i32 1)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 1u);
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, PrunedInterfaceSpillsIntoSummaryBuckets) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;
  IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
  IntraLotusAAConfig::lotus_restrict_inline_size = 100;
  IntraLotusAAConfig::lotus_restrict_ap_level = 0;

  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    define void @set_cb(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i32 (i32)* %cb, i32 (i32)** %f
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("set_cb"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_EQ(PTG->outputs.size(), 1u);
  ASSERT_GE(PTG->summary_outputs.size(), 2u);
  EXPECT_FALSE(PTG->summary_outputs[1]->empty());
}
