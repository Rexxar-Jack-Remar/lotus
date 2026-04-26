#include "Analysis/DebugInfo/UniqueIR/IDToValueMapper.h"
#include "Analysis/DebugInfo/UniqueIR/UniqueIRMarker.h"
#include "Analysis/DebugInfo/UniqueIR/UniqueIRReader.h"
#include "Analysis/DebugInfo/UniqueIR/UniqueIRVerifier.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>

namespace {

using lotus::IDToFunctionMapper;
using lotus::IDToInstructionMapper;
using lotus::UniqueIRMarker;
using lotus::UniqueIRMarkerMode;
using lotus::UniqueIRReader;
using lotus::UniqueIRVerifier;
using lotus::unittest::parseModule;

llvm::Instruction *findInstruction(llvm::Function *function,
                                   llvm::StringRef name) {
  for (auto &basic_block : *function) {
    for (auto &instruction : basic_block) {
      if (instruction.getName() == name) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

TEST(UniqueIRMarkerTest, MarksModuleFunctionBlocksAndInstructions) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @example(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      br label %exit
    exit:
      ret i32 %inc
    }
  )");
  ASSERT_NE(module, nullptr);

  UniqueIRMarker marker(UniqueIRMarkerMode::Instrument);
  EXPECT_TRUE(marker.mark(*module));

  auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  auto *inc = findInstruction(function, "inc");
  ASSERT_NE(inc, nullptr);

  EXPECT_EQ(UniqueIRReader::getModuleID(module.get()), 0U);
  EXPECT_EQ(UniqueIRReader::getFunctionID(function), 0U);
  EXPECT_EQ(UniqueIRReader::getBasicBlockID(&function->getEntryBlock()), 0U);
  EXPECT_TRUE(UniqueIRReader::getInstructionID(inc).has_value());
  EXPECT_TRUE(UniqueIRVerifier().verify(*module));
}

TEST(UniqueIRMarkerTest, MapsSelectedIDsBackToValues) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @example() {
    entry:
      %a = add i32 1, 2
      %b = add i32 %a, 3
      ret i32 %b
    }
  )");
  ASSERT_NE(module, nullptr);

  UniqueIRMarker marker;
  ASSERT_TRUE(marker.mark(*module));

  auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  auto *a = findInstruction(function, "a");
  ASSERT_NE(a, nullptr);

  auto function_id = UniqueIRReader::getFunctionID(function);
  auto instruction_id = UniqueIRReader::getInstructionID(a);
  ASSERT_TRUE(function_id.has_value());
  ASSERT_TRUE(instruction_id.has_value());

  std::set<lotus::UniqueIRID> function_ids{*function_id};
  std::set<lotus::UniqueIRID> instruction_ids{*instruction_id};

  auto function_map = IDToFunctionMapper(*module).idToValueMap(function_ids);
  auto instruction_map = IDToInstructionMapper(*module).idToValueMap(
      instruction_ids);

  ASSERT_EQ(function_map->size(), 1U);
  ASSERT_EQ(instruction_map->size(), 1U);
  EXPECT_EQ(function_map->at(*function_id), function);
  EXPECT_EQ(instruction_map->at(*instruction_id), a);
}

TEST(UniqueIRMarkerTest, MarksLoopsAndPreservesExistingLoopMetadata) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @loop(i32 %n) {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %latch, label %exit
    latch:
      %next = add i32 %i, 1
      br label %header, !llvm.loop !0
    exit:
      ret void
    }

    !0 = distinct !{!0, !1}
    !1 = !{!"llvm.loop.unroll.disable"}
  )");
  ASSERT_NE(module, nullptr);

  llvm::Function *function = module->getFunction("loop");
  ASSERT_NE(function, nullptr);

  llvm::DominatorTree dom_tree(*function);
  llvm::LoopInfo loop_info(dom_tree);
  auto get_loop_info = [&loop_info](llvm::Function &) { return &loop_info; };

  UniqueIRMarker marker;
  EXPECT_TRUE(marker.mark(*module, get_loop_info));
  ASSERT_EQ(loop_info.getLoopsInPreorder().size(), 1U);

  auto *loop = loop_info.getLoopsInPreorder().front();
  EXPECT_EQ(UniqueIRReader::getLoopID(loop), 0U);
  EXPECT_TRUE(UniqueIRVerifier().verify(*module, get_loop_info));

  auto *loop_id = loop->getLoopID();
  ASSERT_NE(loop_id, nullptr);
  bool preserved_unroll_disable = false;
  for (unsigned index = 1; index < loop_id->getNumOperands(); ++index) {
    auto *tuple = llvm::dyn_cast_or_null<llvm::MDTuple>(
        loop_id->getOperand(index));
    if (!tuple || tuple->getNumOperands() == 0) {
      continue;
    }
    auto *name = llvm::dyn_cast_or_null<llvm::MDString>(tuple->getOperand(0));
    preserved_unroll_disable |= name && name->getString() == "llvm.loop.unroll.disable";
  }
  EXPECT_TRUE(preserved_unroll_disable);
}

} // namespace
