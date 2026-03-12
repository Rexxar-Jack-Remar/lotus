#include "Analysis/Concurrency/Utils/ThreadLocalAnalysis.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace ThreadLocal;

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

class ThreadLocalAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("ThreadLocalAnalysisTest", errs());
    }
    return module;
  }
};

TEST_F(ThreadLocalAnalysisTest, StoreThroughStackGepStaysThreadLocal) {
  const char *source = R"(
    %struct.S = type { i32, i32 }

    define i32 @main() {
    entry:
      %obj = alloca %struct.S, align 4
      %field = getelementptr inbounds %struct.S, %struct.S* %obj, i32 0, i32 1
      store i32 7, i32* %field, align 4
      %load = load i32, i32* %field, align 4
      ret i32 %load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *load = findInstructionByName(*main_func, "load");
  ASSERT_NE(load, nullptr);

  EXPECT_TRUE(tla.accessesThreadLocalStorage(load));
}

TEST_F(ThreadLocalAnalysisTest, InvokeGetspecificIsRecognizedAsThreadLocal) {
  const char *source = R"(
    declare i8* @pthread_getspecific(i32)
    declare i32 @__gxx_personality_v0(...)

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %tls = invoke i8* @pthread_getspecific(i32 0)
              to label %cont unwind label %lpad

    cont:
      %use = ptrtoint i8* %tls to i64
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *tls = findInstructionByName(*main_func, "tls");
  ASSERT_NE(tls, nullptr);

  EXPECT_TRUE(tla.isThreadLocal(tls));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
