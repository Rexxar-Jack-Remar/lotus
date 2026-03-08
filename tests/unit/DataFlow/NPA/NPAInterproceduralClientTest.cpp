#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralAffineEqualities.h"
#include "Dataflow/NPA/Analyses/BackwardInterproceduralEngine.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralIntervalAnalysis.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralLiveVariables.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralMaybeUninitialized.h"
#include "Dataflow/NPA/Domains/ProgramTransferDomain.h"

#include <gtest/gtest.h>

#include <iterator>
#include <limits>
#include <llvm/ADT/APInt.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <set>
#include <string>
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

const llvm::Instruction *findInstructionByName(const llvm::Function &function,
                                               llvm::StringRef name) {
  for (const auto &block : function) {
    for (const auto &inst : block) {
      if (inst.hasName() && inst.getName() == name)
        return &inst;
    }
  }
  return nullptr;
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

std::vector<npa::AffineState>
materializedAffineStatesForBlock(
    const std::map<npa::BlockKey, npa::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<npa::AffineState> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(npa::materializeAffineExpressions(entry.second));
  }
  return out;
}

std::vector<const npa::AffineRelationDomain::value_type *>
relationsForBlock(
    const std::map<npa::BlockKey, npa::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<const npa::AffineRelationDomain::value_type *> out;
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

template <typename Op, typename OpLess>
bool containsPath(
    const npa::ProgramTransfer<Op, OpLess> &transfer,
    std::initializer_list<Op> expected) {
  typename npa::ProgramTransfer<Op, OpLess>::path_type path(expected);
  return transfer.paths.count(path) != 0;
}

llvm::APInt signedAPInt(unsigned bitWidth, int64_t value) {
  return llvm::APInt(bitWidth, static_cast<uint64_t>(value), true);
}

llvm::APInt unsignedAPInt(unsigned bitWidth, uint64_t value) {
  return llvm::APInt(bitWidth, value, false);
}

void expectConstValue(const npa::ConstantPropagationValue &value,
                      const llvm::APInt &expected) {
  EXPECT_EQ(value.tag, npa::ConstantPropagationTag::Const);
  EXPECT_EQ(value.constant.getBitWidth(), expected.getBitWidth());
  EXPECT_TRUE(value.constant.eq(expected));
}

void expectIntervalPoint(const npa::Interval &value, const llvm::APInt &expected,
                         npa::IntervalOrdering ordering) {
  EXPECT_FALSE(value.bottom);
  EXPECT_TRUE(value.hasLower);
  EXPECT_TRUE(value.hasUpper);
  EXPECT_EQ(value.ordering, ordering);
  ASSERT_EQ(value.lower.getBitWidth(), expected.getBitWidth());
  ASSERT_EQ(value.upper.getBitWidth(), expected.getBitWidth());
  EXPECT_TRUE(value.lower.eq(expected));
  EXPECT_TRUE(value.upper.eq(expected));
}

void expectIntervalRange(const npa::Interval &value, const llvm::APInt &lower,
                         const llvm::APInt &upper,
                         npa::IntervalOrdering ordering) {
  EXPECT_FALSE(value.bottom);
  EXPECT_TRUE(value.hasLower);
  EXPECT_TRUE(value.hasUpper);
  EXPECT_EQ(value.ordering, ordering);
  ASSERT_EQ(value.lower.getBitWidth(), lower.getBitWidth());
  ASSERT_EQ(value.upper.getBitWidth(), upper.getBitWidth());
  EXPECT_TRUE(value.lower.eq(lower));
  EXPECT_TRUE(value.upper.eq(upper));
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
  expectConstValue(It->second, signedAPInt(32, 5));
}

TEST(NPAInterproceduralClients, ConstantPropagationUsesLLVMIntegerSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %z = zext i1 true to i32
      %t = trunc i32 256 to i8
      %cmp = icmp ugt i32 -1, 1
      %q = udiv i32 -1, 2
      %s = lshr i32 -1, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Z = findInstructionByName(*Main, "z");
  auto *T = findInstructionByName(*Main, "t");
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Q = findInstructionByName(*Main, "q");
  auto *S = findInstructionByName(*Main, "s");
  ASSERT_NE(Z, nullptr);
  ASSERT_NE(T, nullptr);
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto ZIt = states.front()->values.find(Z);
  auto TIt = states.front()->values.find(T);
  auto CmpIt = states.front()->values.find(Cmp);
  auto QIt = states.front()->values.find(Q);
  auto SIt = states.front()->values.find(S);
  ASSERT_NE(ZIt, states.front()->values.end());
  ASSERT_NE(TIt, states.front()->values.end());
  ASSERT_NE(CmpIt, states.front()->values.end());
  ASSERT_NE(QIt, states.front()->values.end());
  ASSERT_NE(SIt, states.front()->values.end());

  expectConstValue(ZIt->second, unsignedAPInt(32, 1));
  expectConstValue(TIt->second, signedAPInt(8, 0));
  expectConstValue(CmpIt->second, unsignedAPInt(1, 1));
  expectConstValue(QIt->second, unsignedAPInt(32, 2147483647));
  expectConstValue(SIt->second, unsignedAPInt(32, 2147483647));
}

TEST(NPAInterproceduralClients, ConstantPropagationSupportsLargeUnsignedValues) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %q = udiv i64 -1, 1
      %s = lshr i64 -1, 0
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Q = findInstructionByName(*Main, "q");
  auto *S = findInstructionByName(*Main, "s");
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto QIt = states.front()->values.find(Q);
  auto SIt = states.front()->values.find(S);
  ASSERT_NE(QIt, states.front()->values.end());
  ASSERT_NE(SIt, states.front()->values.end());
  expectConstValue(QIt->second, unsignedAPInt(64, std::numeric_limits<uint64_t>::max()));
  expectConstValue(SIt->second, unsignedAPInt(64, std::numeric_limits<uint64_t>::max()));
}

TEST(NPAInterproceduralClients, ConstantPropagationPreservesPhiConditionCorrelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %x = phi i32 [ 1, %left ], [ 2, %right ]
      %c = zext i1 %cond to i32
      %y = add i32 %x, %c
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin(), 4);
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Y);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 2));
}

TEST(NPAInterproceduralClients, ConstantPropagationResolvesIndirectSingleTargetCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %fp = select i1 true, i32 (i32)* @id, i32 (i32)* @id
      %r = call i32 %fp(i32 7)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterproceduralConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 7));
}

TEST(NPAInterproceduralClients, ConstantPropagationDefaultSwitchCanBeUnreachable) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      switch i32 3, label %default [ i32 1, label %case1
                                     i32 3, label %case3 ]

    case1:
      %x1 = add i32 1, 1
      br label %join

    case3:
      %x3 = add i32 4, 5
      br label %join

    default:
      %xd = add i32 7, 8
      br label %join

    join:
      %x = phi i32 [ %x1, %case1 ], [ %x3, %case3 ], [ %xd, %default ]
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = findInstructionByName(*Main, "x");
  ASSERT_NE(X, nullptr);
  auto NextIt = std::next(Main->begin(), 4);
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(X);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 9));
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
  expectIntervalRange(It->second, signedAPInt(32, 2), signedAPInt(32, 10),
                      npa::IntervalOrdering::Signed);
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
  auto relations = relationsForBlock(result.blockRelations, &Sink->getEntryBlock());
  ASSERT_EQ(relations.size(), 1u);
  EXPECT_FALSE(relations.front()->bottom);
  auto states =
      materializedAffineStatesForBlock(result.blockRelations, &Sink->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto XIt = states.front().values.find(X);
  auto YValueIt = states.front().values.find(Y);
  ASSERT_NE(XIt, states.front().values.end());
  ASSERT_NE(YValueIt, states.front().values.end());

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

TEST(NPAInterproceduralClients, IntervalCastKeepsZextTrueAsOne) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %v = zext i1 true to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Value = findInstructionByName(*Main, "v");
  ASSERT_NE(Value, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Value);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalPoint(It->second, unsignedAPInt(32, 1),
                      npa::IntervalOrdering::Unsigned);
}

TEST(NPAInterproceduralClients, IntervalSignedDivisionUsesAllEndpointPairs) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %c1, i1 %c2) {
    entry:
      %x = select i1 %c1, i32 -10, i32 5
      %y = select i1 %c2, i32 -2, i32 -1
      %q = sdiv i32 %x, %y
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Quotient = findInstructionByName(*Main, "q");
  ASSERT_NE(Quotient, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Quotient);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalRange(It->second, signedAPInt(32, -5), signedAPInt(32, 10),
                      npa::IntervalOrdering::Signed);
}

TEST(NPAInterproceduralClients, IntervalUnsignedOpsUseUnsignedSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %cmp = icmp ugt i32 -1, 1
      %q = udiv i32 -1, 2
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Quotient = findInstructionByName(*Main, "q");
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Quotient, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto CmpIt = states.front()->values.find(Cmp);
  ASSERT_NE(CmpIt, states.front()->values.end());
  expectIntervalPoint(CmpIt->second, unsignedAPInt(1, 1),
                      npa::IntervalOrdering::Signed);

  auto QuotientIt = states.front()->values.find(Quotient);
  ASSERT_NE(QuotientIt, states.front()->values.end());
  expectIntervalPoint(QuotientIt->second, unsignedAPInt(32, 2147483647),
                      npa::IntervalOrdering::Unsigned);
}

TEST(NPAInterproceduralClients, IntervalSupportsLargeUnsignedValues) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %q = udiv i64 -1, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Q = findInstructionByName(*Main, "q");
  ASSERT_NE(Q, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Q);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalPoint(It->second, unsignedAPInt(64, std::numeric_limits<uint64_t>::max()),
                      npa::IntervalOrdering::Unsigned);
}

TEST(NPAInterproceduralClients, IntervalPreservesPhiConditionCorrelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %x = phi i32 [ 1, %left ], [ 2, %right ]
      %c = zext i1 %cond to i32
      %y = add i32 %x, %c
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin(), 4);
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Y);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalPoint(It->second, signedAPInt(32, 2),
                      npa::IntervalOrdering::Signed);
}

TEST(NPAInterproceduralClients, IntervalDefaultSwitchNarrowsRepresentableRange) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %c1, i1 %c2) {
    entry:
      %x = select i1 %c1, i32 0, i32 2
      %y = select i1 %c2, i32 %x, i32 1
      switch i32 %y, label %default [ i32 0, label %case0 ]

    case0:
      br label %join

    default:
      br label %join

    join:
      %z = phi i32 [ 0, %case0 ], [ %y, %default ]
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto DefaultIt = std::next(Main->begin(), 2);
  ASSERT_NE(DefaultIt, Main->end());
  auto *Default = &*DefaultIt;

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Default);
  ASSERT_EQ(states.size(), 1u);

  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto It = states.front()->values.find(Y);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalRange(It->second, signedAPInt(32, 1), signedAPInt(32, 2),
                      npa::IntervalOrdering::Signed);
}

TEST(NPAInterproceduralClients, AffineDefaultSwitchRemainsUnrefinedByDisequality) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      switch i32 %x, label %default [ i32 0, label %case0 ]

    case0:
      br label %join

    default:
      br label %join

    join:
      %y = phi i32 [ 0, %case0 ], [ %x, %default ]
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto DefaultIt = std::next(Main->begin(), 2);
  ASSERT_NE(DefaultIt, Main->end());
  auto *Default = &*DefaultIt;

  auto result = npa::InterproceduralAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Default);
  ASSERT_EQ(states.size(), 1u);

  auto *X = &*Main->arg_begin();
  auto It = states.front().values.find(X);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  auto XIt = It->second.terms.find(X);
  ASSERT_NE(XIt, It->second.terms.end());
  EXPECT_EQ(XIt->second, 1);
}

TEST(NPAInterproceduralClients, AffineCastAndSelectUseKnownConditionValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %cond = trunc i32 1 to i1
      %x = select i1 %cond, i32 4, i32 7
      %v = zext i1 %cond to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = findInstructionByName(*Main, "x");
  auto *V = findInstructionByName(*Main, "v");
  ASSERT_NE(X, nullptr);
  ASSERT_NE(V, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto XIt = states.front().values.find(X);
  ASSERT_NE(XIt, states.front().values.end());
  EXPECT_FALSE(XIt->second.top);
  EXPECT_TRUE(XIt->second.terms.empty());
  EXPECT_EQ(XIt->second.constant, 4);

  auto VIt = states.front().values.find(V);
  ASSERT_NE(VIt, states.front().values.end());
  EXPECT_FALSE(VIt->second.top);
  EXPECT_TRUE(VIt->second.terms.empty());
  EXPECT_EQ(VIt->second.constant, 1);
}

TEST(NPAInterproceduralClients, AffineCompareOfSameValueProducesConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %cmp = icmp eq i32 %x, %x
      %sel = select i1 %cmp, i32 11, i32 12
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Sel = findInstructionByName(*Main, "sel");
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Sel, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto CmpIt = states.front().values.find(Cmp);
  ASSERT_NE(CmpIt, states.front().values.end());
  EXPECT_FALSE(CmpIt->second.top);
  EXPECT_TRUE(CmpIt->second.terms.empty());
  EXPECT_EQ(CmpIt->second.constant, 1);

  auto SelIt = states.front().values.find(Sel);
  ASSERT_NE(SelIt, states.front().values.end());
  EXPECT_FALSE(SelIt->second.top);
  EXPECT_TRUE(SelIt->second.terms.empty());
  EXPECT_EQ(SelIt->second.constant, 11);
}

TEST(NPAInterproceduralClients, AffinePhiKeepsBranchConditionAtMerge) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %x = phi i32 [ 1, %left ], [ 2, %right ]
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cond = &*Main->arg_begin();
  auto MergeIt = std::next(Main->begin(), 3);
  ASSERT_NE(MergeIt, Main->end());
  auto *Merge = &*MergeIt;
  auto *X = findInstructionByName(*Main, "x");
  ASSERT_NE(X, nullptr);

  auto result = npa::InterproceduralAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Merge);
  ASSERT_EQ(states.size(), 1u);

  auto XIt = states.front().values.find(X);
  ASSERT_NE(XIt, states.front().values.end());
  EXPECT_FALSE(XIt->second.top);
  EXPECT_EQ(XIt->second.constant, 2);
  ASSERT_EQ(XIt->second.terms.size(), 1u);
  auto CondIt = XIt->second.terms.find(Cond);
  ASSERT_NE(CondIt, XIt->second.terms.end());
  EXPECT_EQ(CondIt->second, -1);
}

namespace {

using TraceTransferDomain = npa::ProgramTransferDomain<char>;

class BackwardEdgeTransferAnalysis {
public:
  using FactType = TraceTransferDomain::value_type;
  using E = npa::E0<TraceTransferDomain>;

  FactType getExitValue(const llvm::Function &) const {
    return TraceTransferDomain::one();
  }

  E getTransfer(llvm::Instruction &, E current) const {
    return current;
  }

  FactType getEdgeTransfer(const llvm::Instruction &term,
                           const llvm::BasicBlock &succ) const {
    auto *Branch = llvm::dyn_cast<llvm::BranchInst>(&term);
    if (!Branch || !Branch->isConditional())
      return TraceTransferDomain::one();
    return Branch->getSuccessor(0) == &succ
               ? TraceTransferDomain::singleton('T')
               : TraceTransferDomain::singleton('F');
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return TraceTransferDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return TraceTransferDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return TraceTransferDomain::equal(lhs, rhs);
  }
};

} // namespace

TEST(NPAInterproceduralClients, BackwardEngineAppliesEdgeTransfers) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      ret void

    right:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  BackwardEdgeTransferAnalysis analysis;
  auto result =
      npa::BackwardInterproceduralEngine<TraceTransferDomain,
                                         BackwardEdgeTransferAnalysis>::run(
          *module, analysis);

  auto Facts = statesForBlock(result.blockEntryFacts, &Main->getEntryBlock());
  ASSERT_EQ(Facts.size(), 1u);
  EXPECT_TRUE(containsPath(*Facts.front(), {'T'}));
  EXPECT_TRUE(containsPath(*Facts.front(), {'F'}));
}

TEST(NPAInterproceduralClients, AffineTracksModularWrapForConstants) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %y = add i8 127, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(Y);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_TRUE(It->second.terms.empty());
  EXPECT_EQ(It->second.constant, -128);
}

TEST(NPAInterproceduralClients, AffineZextOfBooleanArgumentStaysSymbolic) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %b) {
    entry:
      %v = zext i1 %b to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Arg = &*Main->arg_begin();
  auto *V = findInstructionByName(*Main, "v");
  ASSERT_NE(V, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(V);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  auto CoeffIt = It->second.terms.find(Arg);
  ASSERT_NE(CoeffIt, It->second.terms.end());
  EXPECT_EQ(CoeffIt->second, 1);
}

TEST(NPAInterproceduralClients, IntervalCompareAndSelectUseForcedRanges) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %c) {
    entry:
      %x = select i1 %c, i32 1, i32 2
      %y = select i1 %c, i32 5, i32 6
      %cmp = icmp slt i32 %x, %y
      %sel = select i1 %cmp, i32 9, i32 10
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Sel = findInstructionByName(*Main, "sel");
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Sel, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterproceduralIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto CmpIt = states.front()->values.find(Cmp);
  ASSERT_NE(CmpIt, states.front()->values.end());
  expectIntervalPoint(CmpIt->second, unsignedAPInt(1, 1),
                      npa::IntervalOrdering::Signed);

  auto SelIt = states.front()->values.find(Sel);
  ASSERT_NE(SelIt, states.front()->values.end());
  expectIntervalPoint(SelIt->second, signedAPInt(32, 9),
                      npa::IntervalOrdering::Signed);
}
