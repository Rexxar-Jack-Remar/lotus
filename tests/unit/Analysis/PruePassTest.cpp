#include "TestUtils/LLVMHelpers.h"
#include "Transform/Nisse/Nisse.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

namespace {

struct PassFixture {
  LoopAnalysisManager lam;
  FunctionAnalysisManager fam;
  CGSCCAnalysisManager cgam;
  ModuleAnalysisManager mam;
  PassBuilder pb;

  PassFixture() {
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    fam.registerPass([] { return nisse::NisseAnalysis(); });
    fam.registerPass([] { return nisse::KSAnalysis(); });
    pb.crossRegisterProxies(lam, fam, cgam, mam);
  }
};

static void runDeltaPass(Module &module) {
  PassFixture fixture;
  nisse::DeltaCounterPass pass;
  pass.run(module, fixture.mam);
}

static void runPruePass(Module &module) {
  PassFixture fixture;
  nisse::PruePass pass;
  pass.run(module, fixture.mam);
}

static GlobalVariable *getCounterArray(Module &module, StringRef name) {
  return module.getNamedGlobal(name);
}

static unsigned countTaggedStores(Function &function) {
  unsigned count = 0;
  for (Instruction &inst : instructions(function)) {
    auto *store = dyn_cast<StoreInst>(&inst);
    if (!store) {
      continue;
    }
    if (store->getMetadata("nisse.prue.update")) {
      ++count;
    }
  }
  return count;
}

static unsigned countStoresToGlobal(Function &function, StringRef global_name) {
  unsigned count = 0;
  for (Instruction &inst : instructions(function)) {
    auto *store = dyn_cast<StoreInst>(&inst);
    if (!store) {
      continue;
    }
    auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
    if (!gep) {
      continue;
    }
    auto *global = dyn_cast<GlobalVariable>(
        gep->getPointerOperand()->stripPointerCasts());
    if (global && global->getName() == global_name) {
      ++count;
    }
  }
  return count;
}

static bool hasStoreInBlock(Function &function, StringRef block_name,
                            StringRef global_name) {
  for (BasicBlock &bb : function) {
    if (bb.getName() != block_name) {
      continue;
    }
    return llvm::any_of(bb, [&](Instruction &inst) {
      auto *store = dyn_cast<StoreInst>(&inst);
      if (!store) {
        return false;
      }
      auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
      if (!gep) {
        return false;
      }
      auto *global = dyn_cast<GlobalVariable>(
          gep->getPointerOperand()->stripPointerCasts());
      return global && global->getName() == global_name;
    });
  }
  return false;
}

TEST(PruePassTest, DeltaInstrumentationCreatesLocalCountersAndTaggedUpdates) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @instrument_me(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      br label %exit

    else:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  runDeltaPass(*module);

  Function *function = module->getFunction("instrument_me");
  ASSERT_NE(function, nullptr);
  EXPECT_NE(getCounterArray(*module, "prue-counter-array"), nullptr);

  unsigned alloca_count = 0;
  for (Instruction &inst : instructions(*function)) {
    auto *alloca = dyn_cast<AllocaInst>(&inst);
    if (alloca && alloca->getName().startswith("prue.delta.")) {
      ++alloca_count;
    }
  }

  EXPECT_GT(alloca_count, 0u);
  EXPECT_GT(countTaggedStores(*function), 0u);
}

TEST(PruePassTest, EliminatesZeroUpdate) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @zero_update() {
    entry:
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, 0
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("zero_update");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 0u);
}

TEST(PruePassTest, SplitsPhiUpdateAndEliminatesZeroArm) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @split_update(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %exit

    then:
      br label %exit

    exit:
      %d = phi i64 [ 0, %entry ], [ 1, %then ]
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("split_update");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 1u);
  EXPECT_FALSE(hasStoreInBlock(*function, "exit", "prue-counter-array"));
}

TEST(PruePassTest, RelocatesUpdateToDefinitionBlock) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @relocate_update(i64 %x) {
    entry:
      br label %then

    then:
      %d = add i64 %x, 1
      br label %exit

    exit:
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("relocate_update");
  ASSERT_NE(function, nullptr);
  EXPECT_TRUE(hasStoreInBlock(*function, "then", "prue-counter-array"));
  EXPECT_FALSE(hasStoreInBlock(*function, "exit", "prue-counter-array"));
}

} // namespace
