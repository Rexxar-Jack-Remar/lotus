#include "Verification/Sifa/CallGraph.h"
#include "Verification/Sifa/Sifa.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>

namespace {

static llvm::BasicBlock *getBlockByName(llvm::Function &F, const char *name) {
  for (llvm::BasicBlock &BB : F) {
    if (BB.getName() == name) {
      return &BB;
    }
  }
  return nullptr;
}

TEST(SifaInterproceduralReachability, ReachesLoiInsideDirectCallee) {
  const char *ir = R"IR(
    declare void @__VERIFIER_error()

    define void @g() {
    entry:
      call void @__VERIFIER_error()
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *entry = getBlockByName(*gFn, "entry");
  ASSERT_NE(entry, nullptr);

  EXPECT_TRUE(lotus::sifa::isReachableInterprocedural(*M, mainFn, *gFn, *entry));
}

TEST(SifaInterproceduralReachability, ReachesCalleeFromBranchingCallBlock) {
  const char *ir = R"IR(
    declare void @__VERIFIER_error()
    declare i1 @nd()

    define void @g() {
    entry:
      call void @__VERIFIER_error()
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      %c = call i1 @nd()
      br i1 %c, label %left, label %right

    left:
      ret void

    right:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  llvm::BasicBlock *entry = getBlockByName(*gFn, "entry");
  ASSERT_NE(entry, nullptr);

  EXPECT_TRUE(lotus::sifa::isReachableInterprocedural(*M, mainFn, *gFn, *entry));
}

TEST(SifaInterproceduralReachability, DetectsUnreachableNoReturnBlocksAsErrorLocations) {
  const char *ir = R"IR(
    declare void @panic() noreturn

    define void @main() {
    entry:
      call void @panic()
      unreachable
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  auto lois = lotus::sifa::CallGraph::gatherErrorLocations(*M);
  ASSERT_EQ(lois.size(), 1u);
  ASSERT_NE(lois.front().first, nullptr);
  ASSERT_NE(lois.front().second, nullptr);
  EXPECT_EQ(lois.front().first->getName(), "main");
  EXPECT_EQ(lois.front().second->getName(), "entry");
}

} // namespace
