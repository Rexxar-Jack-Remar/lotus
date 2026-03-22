#include "Analysis/Concurrency/Memory/MemUseDefAnalysis.h"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;

static const Instruction *findInstructionByName(const Function &func,
                                                StringRef name) {
  for (const BasicBlock &bb : func) {
    for (const Instruction &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

class MemUseDefAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("MemUseDefAnalysisTest", errs());
    }
    return module;
  }
};

TEST_F(MemUseDefAnalysisTest, LocalLoadMapsBackToUnderlyingMemory) {
  const char *source = R"(
    @g = global i32 0, align 4

    define i32 @main() {
    entry:
      store i32 7, i32* @g, align 4
      %ld = load i32, i32* @g, align 4
      ret i32 %ld
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  AssumptionCache AC(*main_func);
  DominatorTree DT(*main_func);
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI(TLII);
  AAResults AAR(TLI);
  BasicAAResult BAA(module->getDataLayout(), *main_func, TLI, AC, &DT);
  AAR.addAAResult(BAA);
  MemorySSA MSSA(*main_func, &AAR, &DT);
  MemoryLdStMapClass result;
  MemoryLdStMapClass::OmpDiagnosticsLocationInfo.initFunc(*main_func);
  MemorySSAUseDefWalker walker(MSSA, result);
  walker.reachingDefAnalysis();

  const Instruction *load_inst = findInstructionByName(*main_func, "ld");
  ASSERT_NE(load_inst, nullptr);

  const Value *memory = result.getMemoryForLdSt(load_inst);
  ASSERT_NE(memory, nullptr);
  EXPECT_TRUE(isa<GlobalVariable>(memory));
}

TEST_F(MemUseDefAnalysisTest, StoreThroughFormalTracksArgumentMemory) {
  const char *source = R"(
    define void @store_through_arg(i32* %p) {
    entry:
      store i32 1, i32* %p, align 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *func = module->getFunction("store_through_arg");
  ASSERT_NE(func, nullptr);

  AssumptionCache AC(*func);
  DominatorTree DT(*func);
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI(TLII);
  AAResults AAR(TLI);
  BasicAAResult BAA(module->getDataLayout(), *func, TLI, AC, &DT);
  AAR.addAAResult(BAA);
  MemorySSA MSSA(*func, &AAR, &DT);
  MemoryLdStMapClass result;
  MemoryLdStMapClass::OmpDiagnosticsLocationInfo.initFunc(*func);
  MemorySSAUseDefWalker walker(MSSA, result);
  walker.reachingDefAnalysis();

  const Instruction *store_inst = nullptr;
  for (const Instruction &inst : instructions(*func)) {
    if (isa<StoreInst>(&inst)) {
      store_inst = &inst;
      break;
    }
  }
  ASSERT_NE(store_inst, nullptr);

  const Value *memory = result.getMemoryForLdSt(store_inst);
  ASSERT_NE(memory, nullptr);
  EXPECT_TRUE(isa<Argument>(memory));
}

TEST_F(MemUseDefAnalysisTest, PointerLoadIsRetainedAsMemoryUse) {
  const char *source = R"(
    @g = global i32 0, align 4
    @ptr = global i32* @g, align 8

    define i32 @main() {
    entry:
      %p = load i32*, i32** @ptr, align 8
      %v = load i32, i32* %p, align 4
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  AssumptionCache AC(*main_func);
  DominatorTree DT(*main_func);
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI(TLII);
  AAResults AAR(TLI);
  BasicAAResult BAA(module->getDataLayout(), *main_func, TLI, AC, &DT);
  AAR.addAAResult(BAA);
  MemorySSA MSSA(*main_func, &AAR, &DT);
  MemoryLdStMapClass result;
  MemoryLdStMapClass::OmpDiagnosticsLocationInfo.initFunc(*main_func);
  MemorySSAUseDefWalker walker(MSSA, result);
  walker.reachingDefAnalysis();

  const Instruction *ptr_load = findInstructionByName(*main_func, "p");
  const Instruction *val_load = findInstructionByName(*main_func, "v");
  ASSERT_NE(ptr_load, nullptr);
  ASSERT_NE(val_load, nullptr);

  const Value *ptr_memory = result.getMemoryForLdSt(ptr_load);
  const Value *val_memory = result.getMemoryForLdSt(val_load);
  ASSERT_NE(ptr_memory, nullptr);
  ASSERT_NE(val_memory, nullptr);
  EXPECT_TRUE(isa<GlobalVariable>(ptr_memory));
  EXPECT_TRUE(isa<GlobalVariable>(val_memory));
}

TEST_F(MemUseDefAnalysisTest, StoreThroughLoadedPointerRetainsMemoryIdentity) {
  const char *source = R"(
    @g = global i32 0, align 4
    @ptr = global i32* @g, align 8

    define void @main() {
    entry:
      %p = load i32*, i32** @ptr, align 8
      store i32 5, i32* %p, align 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  AssumptionCache AC(*main_func);
  DominatorTree DT(*main_func);
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI(TLII);
  AAResults AAR(TLI);
  BasicAAResult BAA(module->getDataLayout(), *main_func, TLI, AC, &DT);
  AAR.addAAResult(BAA);
  MemorySSA MSSA(*main_func, &AAR, &DT);
  MemoryLdStMapClass result;
  MemoryLdStMapClass::OmpDiagnosticsLocationInfo.initFunc(*main_func);
  MemorySSAUseDefWalker walker(MSSA, result);
  walker.reachingDefAnalysis();

  const Instruction *ptr_load = findInstructionByName(*main_func, "p");
  ASSERT_NE(ptr_load, nullptr);

  const Instruction *store_inst = nullptr;
  for (const Instruction &inst : instructions(*main_func)) {
    if (isa<StoreInst>(&inst) && inst.getOperand(1) == ptr_load) {
      store_inst = &inst;
      break;
    }
  }
  ASSERT_NE(store_inst, nullptr);

  const Value *store_memory = result.getMemoryForLdSt(store_inst);
  ASSERT_NE(store_memory, nullptr);
  EXPECT_TRUE(isa<GlobalVariable>(store_memory));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
