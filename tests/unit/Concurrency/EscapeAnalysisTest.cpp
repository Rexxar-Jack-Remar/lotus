#include "Analysis/Concurrency/Memory/EscapeAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace lotus;

class EscapeAnalysisTest : public lotus::unittest::LlvmModuleTest {};

TEST_F(EscapeAnalysisTest, GlobalIsEscaped) {
  const char *source = R"(
    @g = global i32 0, align 4

    define void @main() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const GlobalVariable *g = module->getNamedGlobal("g");
  ASSERT_NE(g, nullptr);
  EXPECT_TRUE(analysis.isEscaped(g));
  EXPECT_FALSE(analysis.isThreadLocal(g));
}

TEST_F(EscapeAnalysisTest, PthreadCreateArgEscaped) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* %arg
    }

    define i32 @main() {
    entry:
      %slot = alloca i8, align 1
      call i32 @pthread_create(i8* null, i8* null, i8* (i8*)* @worker, i8* %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *alloca = &main_func->getEntryBlock().front();
  ASSERT_NE(alloca, nullptr);
  EXPECT_TRUE(analysis.isEscaped(alloca));
}

TEST_F(EscapeAnalysisTest, ExternalCallEscapesStackAddress) {
  const char *source = R"(
    declare void @external_sink(i8*)

    define void @main() {
    entry:
      %slot = alloca i8, align 1
      call void @external_sink(i8* %slot)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(analysis.isEscaped(slot));
}

TEST_F(EscapeAnalysisTest, IndirectCallEscapesStackAddress) {
  const char *source = R"(
    declare void @sink(i8*)

    define void @main() {
    entry:
      %slot = alloca i8, align 1
      %fn = select i1 true, void (i8*)* @sink, void (i8*)* @sink
      call void %fn(i8* %slot)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(analysis.isEscaped(slot));
}

TEST_F(EscapeAnalysisTest, CppThreadLikeForkEscapesPointerPayload) {
  const char *source = R"(
    declare void @_ZNSt6threadC1EPFvPvES2_(i8*, void (i8*)*, i8*)

    define void @worker(i8* %arg) {
    entry:
      ret void
    }

    define void @main() {
    entry:
      %thread_obj = alloca i8, align 1
      %payload = alloca i8, align 1
      call void @_ZNSt6threadC1EPFvPvES2_(i8* %thread_obj,
                                          void (i8*)* @worker,
                                          i8* %payload)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  ++it;
  const auto *payload = dyn_cast<AllocaInst>(&*it);
  ASSERT_NE(payload, nullptr);
  EXPECT_TRUE(analysis.isEscaped(payload));
}

TEST_F(EscapeAnalysisTest, ThreadHandleStorageDoesNotEscapeAtForkSite) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *tid = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(tid, nullptr);
  EXPECT_FALSE(analysis.isEscaped(tid));
}

TEST_F(EscapeAnalysisTest, ReturnedButUnsharedPointerStaysThreadLocal) {
  const char *source = R"(
    define i8* @identity(i8* %arg) {
    entry:
      ret i8* %arg
    }

    define void @main() {
    entry:
      %slot = alloca i8, align 1
      %ret = call i8* @identity(i8* %slot)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(analysis.isEscaped(slot));
}

TEST_F(EscapeAnalysisTest, BitcastedInternalCallPropagatesEscape) {
  const char *source = R"(
    declare void @external_sink(i8*)

    define i8* @id(i8* %arg) {
    entry:
      ret i8* %arg
    }

    define void @main() {
    entry:
      %slot = alloca i8, align 1
      %callee = bitcast i8* (i8*)* @id to i8* (i8*)*
      %ret = call i8* %callee(i8* %slot)
      call void @external_sink(i8* %ret)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(analysis.isEscaped(slot));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
