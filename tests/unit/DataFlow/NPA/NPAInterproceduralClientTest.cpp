#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralAffineEqualities.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralIntervalAnalysis.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralLiveVariables.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralMaybeUninitialized.h"

#include <gtest/gtest.h>

#include <llvm/ADT/APInt.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <set>
#include <vector>

namespace {

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &ctx,
                                          const char *ir) {
  llvm::SMDiagnostic err;
  auto module = llvm::parseAssemblyString(ir, err, ctx);
  if (!module)
    err.print("NPAInterproceduralClientTest", llvm::errs());
  return module;
}

template <typename T>
std::vector<const T *> statesForBlock(const std::map<npa::BlockKey, T> &facts,
                                      const llvm::BasicBlock *block) {
  std::vector<const T *> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(&entry.second);
  }
  return out;
}

llvm::APInt unionFactForBlock(const std::map<npa::BlockKey, llvm::APInt> &facts,
                              const llvm::BasicBlock *block) {
  bool found = false;
  llvm::APInt fact(1, 0);
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    if (!found) {
      fact = entry.second;
      found = true;
    } else {
      fact |= entry.second;
    }
  }
  EXPECT_TRUE(found);
  return fact;
}

} // namespace

TEST(NPAInterproceduralClients, MaybeUninitializedFlowsThroughCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = alloca i32
      %r = call i32 @id(i32* %p)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  auto result = npa::InterproceduralMaybeUninitialized::run(*module);
  llvm::APInt entryFact = unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_GT(entryFact.countPopulation(), 0u);
}

TEST(NPAInterproceduralClients, MaybeUninitializedStoreClearsBeforeCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = alloca i32
      store i32 7, i32* %p
      %r = call i32 @id(i32* %p)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  auto result = npa::InterproceduralMaybeUninitialized::run(*module);
  llvm::APInt entryFact = unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_EQ(entryFact.countPopulation(), 0u);
}

TEST(NPAInterproceduralClients, ConstantPropagationTransfersArgumentsAcrossCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @add2(i32 %x) {
    entry:
      %y = add i32 %x, 2
      ret i32 %y
    }

    define i32 @main() {
    entry:
      %r = call i32 @add2(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Add2 = module->getFunction("add2");
  ASSERT_NE(Add2, nullptr);
  auto *Arg = &*Add2->arg_begin();

  auto result = npa::InterproceduralConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, &Add2->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  EXPECT_EQ(It->second.tag, npa::ConstantPropagationTag::Const);
  EXPECT_EQ(It->second.constant, 5);
}

TEST(NPAInterproceduralClients, IntervalAnalysisJoinsAtSingleFunctionEntry) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %a = call i32 @id(i32 2)
      %b = call i32 @id(i32 10)
      %c = add i32 %a, %b
      ret i32 %c
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  EXPECT_TRUE(It->second.hasLower);
  EXPECT_TRUE(It->second.hasUpper);
  EXPECT_EQ(It->second.lower, 2);
  EXPECT_EQ(It->second.upper, 10);
}

TEST(NPAInterproceduralClients, AffineEqualitiesTransferSymbolicRelations) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @sink(i32 %x, i32 %y) {
    entry:
      ret void
    }

    define void @caller(i32 %a) {
    entry:
      %b = add i32 %a, 4
      call void @sink(i32 %a, i32 %b)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Sink = module->getFunction("sink");
  auto *Caller = module->getFunction("caller");
  ASSERT_NE(Sink, nullptr);
  ASSERT_NE(Caller, nullptr);
  auto *X = &*Sink->arg_begin();
  auto YIt = Sink->arg_begin();
  ++YIt;
  auto *Y = &*YIt;
  auto *A = &*Caller->arg_begin();

  auto result = npa::InterproceduralAffineEqualities::run(*module);
  auto states = statesForBlock(result.blockFacts, &Sink->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto XIt = states.front()->values.find(X);
  auto YValueIt = states.front()->values.find(Y);
  ASSERT_NE(XIt, states.front()->values.end());
  ASSERT_NE(YValueIt, states.front()->values.end());

  EXPECT_FALSE(XIt->second.top);
  EXPECT_EQ(XIt->second.constant, 0);
  EXPECT_EQ(XIt->second.terms.size(), 1u);
  auto XCoeffIt = XIt->second.terms.find(A);
  ASSERT_NE(XCoeffIt, XIt->second.terms.end());
  EXPECT_EQ(XCoeffIt->second, 1);

  EXPECT_FALSE(YValueIt->second.top);
  EXPECT_EQ(YValueIt->second.constant, 4);
  EXPECT_EQ(YValueIt->second.terms.size(), 1u);
  auto YCoeffIt = YValueIt->second.terms.find(A);
  ASSERT_NE(YCoeffIt, YValueIt->second.terms.end());
  EXPECT_EQ(YCoeffIt->second, 1);
}

TEST(NPAInterproceduralClients, LiveVariablesFlowBackThroughCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %r = call i32 @id(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterproceduralLiveVariables::run(*module);
  auto BitIt = result.valueBits.find(Arg);
  ASSERT_NE(BitIt, result.valueBits.end());

  llvm::APInt liveIn = unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_TRUE(liveIn[BitIt->second]);
}

TEST(NPAInterproceduralClients, RecursiveIntervalAnalysisConverges) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @g(i32 %x) {
    entry:
      %cmp = icmp sle i32 %x, 0
      br i1 %cmp, label %base, label %step

    base:
      ret i32 %x

    step:
      %dec = sub i32 %x, 1
      %r = call i32 @f(i32 %dec)
      ret i32 %r
    }

    define i32 @f(i32 %x) {
    entry:
      %cmp = icmp sle i32 %x, 0
      br i1 %cmp, label %base, label %step

    base:
      ret i32 %x

    step:
      %dec = sub i32 %x, 1
      %r = call i32 @g(i32 %dec)
      ret i32 %r
    }

    define i32 @main() {
    entry:
      %r = call i32 @f(i32 3)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *Arg = &*F->arg_begin();

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, &F->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  EXPECT_TRUE(states.front()->reachable);
  auto It = states.front()->values.find(Arg);
  if (It != states.front()->values.end())
    EXPECT_TRUE(It->second.hasLower || It->second.hasUpper);
}
