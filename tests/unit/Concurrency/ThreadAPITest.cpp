#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;

class ThreadAPITest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("ThreadAPITest", errs());
    }
    return module;
  }
};

TEST_F(ThreadAPITest, ParsesExtendedTypeNames) {
  EXPECT_EQ(ThreadAPI::stringToType("TD_CANCEL"), ThreadAPI::TD_CANCEL);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_BARRIER"),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(ThreadAPI::stringToType("TD_SHARED_LOCK_DTOR"),
            ThreadAPI::TD_SHARED_LOCK_DTOR);
}

TEST_F(ThreadAPITest, PthreadCancelIsNotClassifiedAsJoin) {
  const char *source = R"(
    declare i32 @pthread_cancel(i8*)

    define i32 @main(i8* %tid) {
    entry:
      %cancel = call i32 @pthread_cancel(i8* %tid)
      ret i32 %cancel
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *cancel_func = module->getFunction("pthread_cancel");
  ASSERT_NE(cancel_func, nullptr);
  EXPECT_EQ(api->getType(cancel_func), ThreadAPI::TD_CANCEL);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *cancel_call = &main_func->getEntryBlock().front();
  EXPECT_FALSE(api->isTDJoin(cancel_call));
}

TEST_F(ThreadAPITest, DistinguishesBlockingAndNonBlockingMPICollectives) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %req) {
    entry:
      %bar = call i32 @MPI_Barrier(i8* %comm)
      %ibar = call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      %bcast = call i32 @MPI_Bcast(i8* null, i32 0, i32 0, i32 0, i8* %comm)
      %ibcast = call i32 @MPI_Ibcast(i8* null, i32 0, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *bar = &*it++;
  const Instruction *ibar = &*it++;
  const Instruction *bcast = &*it++;
  const Instruction *ibcast = &*it++;

  EXPECT_TRUE(api->isBlockingMPIBarrier(bar));
  EXPECT_TRUE(api->isNonBlockingMPIBarrier(ibar));
  EXPECT_TRUE(api->isBlockingMPICollective(bcast));
  EXPECT_TRUE(api->isNonBlockingMPICollective(ibcast));
}
