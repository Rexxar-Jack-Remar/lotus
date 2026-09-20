#include "Alias/InclusionBased/GPG/Analysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <set>

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

using namespace lotus::gpg;
using namespace lotus::unittest;

namespace {

const GPU *findGPU(const GPG &graph, StatementId statement, GPUKind kind,
                   LocationId target) {
  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    for (const GPU &gpu : block.gpus) {
      if (gpu.statement == statement && gpu.kind == kind &&
          gpu.target.location == target)
        return &gpu;
    }
  }
  return nullptr;
}

} // namespace

TEST(GPGFrontendRegression, LoadSnapshotSurvivesAnInterveningStore) {
  const char *ir = R"(
    define i8* @snapshot(i1 %condition) {
    entry:
      %first = alloca i8
      %second = alloca i8
      %cell = alloca i8*
      store i8* %first, i8** %cell
      %loaded = load i8*, i8** %cell
      store i8* %second, i8** %cell
      %wide = bitcast i8* %loaded to i32*
      %narrow = bitcast i32* %wide to i8*
      %selected = select i1 %condition, i8* %narrow, i8* %loaded
      ret i8* %selected
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGFrontendRegression");
  ASSERT_NE(module, nullptr);
  llvm::Function *function = module->getFunction("snapshot");
  ASSERT_NE(function, nullptr);

  ProgramModel model(*module);
  GPGConfig config;
  LLVMFrontend frontend(model, config);
  llvm::Instruction *loaded = findInstructionByName(function, "loaded");
  llvm::Instruction *selected = findInstructionByName(function, "selected");
  llvm::Instruction *first = findInstructionByName(function, "first");
  llvm::Instruction *second = findInstructionByName(function, "second");
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(selected, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  const std::vector<Access> selected_accesses = frontend.readAccesses(selected);
  ASSERT_EQ(selected_accesses.size(), 1u);
  EXPECT_EQ(selected_accesses.front().location, model.valueLocation(loaded));
  EXPECT_EQ(selected_accesses.front().indirections,
            IndirectionList::dereferences(1));

  GPG graph = frontend.buildInitialGPG(*function);
  ASSERT_TRUE(graph.validate());
  for (const GPU &support : graph.supportGPUs())
    EXPECT_NE(support.source.location, model.valueLocation(loaded));

  auto compatible = [&](const llvm::Type *lhs, const llvm::Type *rhs) {
    return model.typesCompatible(lhs, rhs);
  };
  ASSERT_TRUE(graph.strengthReduce(compatible, config.heap_indirection_limit));

  auto *return_instruction = llvm::dyn_cast<llvm::ReturnInst>(
      function->getEntryBlock().getTerminator());
  ASSERT_NE(return_instruction, nullptr);
  const GPU *return_gpu = findGPU(graph, model.statementId(return_instruction),
                                  GPUKind::Return, model.objectLocation(first));
  ASSERT_NE(return_gpu, nullptr);
  EXPECT_EQ(findGPU(graph, model.statementId(return_instruction),
                    GPUKind::Return, model.objectLocation(second)),
            nullptr);
}

TEST(GPGFrontendRegression, CyclicSSADefUseIsKLimited) {
  const char *ir = R"(
    define i8* @walk(i8* %head, i1 %again) {
    entry:
      br label %loop
    loop:
      %cursor = phi i8* [ %head, %entry ], [ %next, %loop ]
      %next = getelementptr i8, i8* %cursor, i64 1
      br i1 %again, label %loop, label %exit
    exit:
      ret i8* %cursor
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGFrontendRegression");
  ASSERT_NE(module, nullptr);
  llvm::Function *function = module->getFunction("walk");
  ASSERT_NE(function, nullptr);

  ProgramModel model(*module);
  GPGConfig config;
  LLVMFrontend frontend(model, config);
  llvm::Instruction *cursor = findInstructionByName(function, "cursor");
  llvm::Instruction *next = findInstructionByName(function, "next");
  ASSERT_NE(cursor, nullptr);
  ASSERT_NE(next, nullptr);

  const std::vector<Access> accesses = frontend.readAccesses(cursor);
  ASSERT_FALSE(accesses.empty());
  EXPECT_TRUE(model.requiresKLimiting(model.valueLocation(cursor)));
  EXPECT_TRUE(model.requiresKLimiting(model.valueLocation(next)));
  EXPECT_TRUE(
      std::all_of(accesses.begin(), accesses.end(), [&](const Access &access) {
        return access.indirections.size() <= config.heap_indirection_limit;
      }));

  GPG graph = frontend.buildInitialGPG(*function);
  ASSERT_TRUE(graph.validate());
  for (const GPU &support : graph.supportGPUs()) {
    EXPECT_LE(support.source.indirections.size(),
              config.heap_indirection_limit);
    EXPECT_LE(support.target.indirections.size(),
              config.heap_indirection_limit);
  }
}

TEST(GPGFrontendRegression, ExcludesEntryUnreachableBasicBlocks) {
  const char *ir = R"(
    define i8* @reachable() {
    entry:
      %live = alloca i8
      ret i8* %live
    dead:
      %dead.object = alloca i8
      %dead.cell = alloca i8*
      store i8* %dead.object, i8** %dead.cell
      ret i8* %dead.object
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGFrontendRegression");
  ASSERT_NE(module, nullptr);
  llvm::Function *function = module->getFunction("reachable");
  ASSERT_NE(function, nullptr);
  llvm::BasicBlock *dead = function->getEntryBlock().getNextNode();
  ASSERT_NE(dead, nullptr);

  ProgramModel model(*module);
  GPGConfig config;
  LLVMFrontend frontend(model, config);
  GPG graph = frontend.buildInitialGPG(*function);
  ASSERT_TRUE(graph.validate());

  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    EXPECT_NE(block.origin_block, dead);
    for (const GPU &gpu : block.gpus)
      EXPECT_NE(gpu.origin ? gpu.origin->getParent() : nullptr, dead);
  }
  for (const GPU &gpu : graph.supportGPUs())
    EXPECT_NE(gpu.origin ? gpu.origin->getParent() : nullptr, dead);

  llvm::Instruction *dead_object =
      findInstructionByName(function, "dead.object");
  ASSERT_NE(dead_object, nullptr);
  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    for (const GPU &gpu : block.gpus) {
      EXPECT_NE(gpu.source.location, model.objectLocation(dead_object));
      EXPECT_NE(gpu.target.location, model.objectLocation(dead_object));
    }
  }
}

TEST(GPGFrontendRegression, CanonicalSSAReadsPreserveValueQueryProvenance) {
  const char *ir = R"(
    define i32 @callee(i32 %value) {
    entry:
      ret i32 %value
    }

    define i32 @main() {
    entry:
      %cell = alloca i32 (i32)*
      store i32 (i32)* @callee, i32 (i32)** %cell
      %same = getelementptr i32 (i32)*, i32 (i32)** %cell, i64 0
      %loaded = load i32 (i32)*, i32 (i32)** %same
      %chosen = select i1 true, i32 (i32)* %loaded,
                                i32 (i32)* %loaded
      %result = call i32 %chosen(i32 7)
      ret i32 %result
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGFrontendRegression");
  ASSERT_NE(module, nullptr);
  llvm::Function *main = module->getFunction("main");
  llvm::Function *callee = module->getFunction("callee");
  ASSERT_NE(main, nullptr);
  ASSERT_NE(callee, nullptr);
  llvm::Instruction *cell = findInstructionByName(main, "cell");
  llvm::Instruction *same = findInstructionByName(main, "same");
  llvm::Instruction *chosen = findInstructionByName(main, "chosen");
  llvm::Instruction *result = findInstructionByName(main, "result");
  ASSERT_NE(cell, nullptr);
  ASSERT_NE(same, nullptr);
  ASSERT_NE(chosen, nullptr);
  auto *indirect_call = llvm::dyn_cast_or_null<llvm::CallBase>(result);
  ASSERT_NE(indirect_call, nullptr);

  GPGAnalysisEngine analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.result().allPointees(cell),
            (std::set<const llvm::Value *>{cell}));
  EXPECT_EQ(analysis.result().allPointees(same),
            (std::set<const llvm::Value *>{cell}));
  EXPECT_EQ(analysis.result().pointees(indirect_call, chosen),
            (std::set<const llvm::Value *>{callee}));
}
