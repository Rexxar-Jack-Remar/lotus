/**
 * @file ICFGTest.cpp
 * @brief Unit tests for Interprocedural Control Flow Graph (ICFG)
 */

#include "IR/ICFG/ICFG.h"

#include "IR/ICFG/ICFGBuilder.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;

class ICFGTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("ICFGTest", errs());
    }
    return module;
  }
};

// Test basic ICFG construction for a simple function
TEST_F(ICFGTest, SimpleFunction) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %x = add i32 1, 2
      br label %exit
    exit:
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("main");
  ASSERT_NE(F, nullptr);

  // Should have nodes for each basic block
  bool foundEntry = false, foundExit = false;
  for (auto &BB : *F) {
    IntraBlockNode *node = icfg.getIntraBlockNode(&BB);
    ASSERT_NE(node, nullptr);
    if (BB.getName() == "entry" || BB.isEntryBlock()) {
      foundEntry = true;
    }
    if (BB.getName() == "exit") {
      foundExit = true;
    }
  }

  EXPECT_TRUE(foundEntry);
  EXPECT_TRUE(foundExit);
}

// Test interprocedural edges for function calls
TEST_F(ICFGTest, FunctionCall) {
  const char *source = R"(
    define i32 @callee() {
      ret i32 42
    }
    
    define i32 @caller() {
      %result = call i32 @callee()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  // Find the call instruction
  CallBase *call = nullptr;
  for (auto &BB : *caller) {
    for (auto &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        call = CB;
        break;
      }
    }
    if (call)
      break;
  }
  ASSERT_NE(call, nullptr);

  // Should have interprocedural edges
  IntraBlockNode *callerNode = icfg.getIntraBlockNode(call->getParent());
  IntraBlockNode *calleeNode = icfg.getIntraBlockNode(&callee->getEntryBlock());
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeNode, nullptr);

  ICFGEdge *callEdge =
      icfg.getICFGEdge(callerNode, calleeNode, ICFGEdge::CallCF);
  EXPECT_NE(callEdge, nullptr);
  EXPECT_NE(callerNode, nullptr);
  EXPECT_NE(calleeNode, nullptr);
}

TEST_F(ICFGTest, IntraEdgeCountForBranch) {
  const char *source = R"(
    define i32 @main(i32 %cond) {
    entry:
      %cmp = icmp eq i32 %cond, 0
      br i1 %cmp, label %then, label %else
    then:
      br label %exit
    else:
      br label %exit
    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("main");
  ASSERT_NE(F, nullptr);

  const BasicBlock *entry = &F->getEntryBlock();
  auto findBlock = [&](StringRef name) -> const BasicBlock * {
    for (const auto &BB : *F) {
      if (BB.getName() == name) {
        return &BB;
      }
    }
    return nullptr;
  };
  const BasicBlock *thenBB = findBlock("then");
  const BasicBlock *elseBB = findBlock("else");
  const BasicBlock *exitBB = findBlock("exit");

  ASSERT_NE(thenBB, nullptr);
  ASSERT_NE(elseBB, nullptr);
  ASSERT_NE(exitBB, nullptr);

  IntraBlockNode *entryNode = icfg.getIntraBlockNode(entry);
  IntraBlockNode *thenNode = icfg.getIntraBlockNode(thenBB);
  IntraBlockNode *elseNode = icfg.getIntraBlockNode(elseBB);
  IntraBlockNode *exitNode = icfg.getIntraBlockNode(exitBB);

  ASSERT_NE(entryNode, nullptr);
  ASSERT_NE(thenNode, nullptr);
  ASSERT_NE(elseNode, nullptr);
  ASSERT_NE(exitNode, nullptr);

  EXPECT_NE(icfg.getICFGEdge(entryNode, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(entryNode, elseNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(thenNode, exitNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(elseNode, exitNode, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, ReturnEdgeFromCallee) {

  const char *source = R"(
    define i32 @callee() {
    entry:
      ret i32 1
    }

    define i32 @caller() {
    entry:
      %result = call i32 @callee()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const BasicBlock *callerEntry = &caller->getEntryBlock();
  const BasicBlock *calleeEntry = &callee->getEntryBlock();
  const BasicBlock *calleeExit = calleeEntry;

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(callerEntry);
  IntraBlockNode *calleeEntryNode = icfg.getIntraBlockNode(calleeEntry);
  IntraBlockNode *calleeExitNode = icfg.getIntraBlockNode(calleeExit);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntryNode, nullptr);
  ASSERT_NE(calleeExitNode, nullptr);

  ICFGEdge *callEdge =
      icfg.getICFGEdge(callerNode, calleeEntryNode, ICFGEdge::CallCF);
  EXPECT_NE(callEdge, nullptr);

  ICFGEdge *retEdge =
      icfg.getICFGEdge(calleeExitNode, callerNode, ICFGEdge::RetCF);
  EXPECT_NE(retEdge, nullptr);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
