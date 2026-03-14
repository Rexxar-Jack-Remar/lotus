#include "Analysis/Concurrency/Memory/StaticThreadSharingAnalysis.h"

#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/InitializePasses.hh"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
using namespace lotus;

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

class StaticSharingProbePass : public ModulePass {
public:
  enum class QueryKind { Instruction, Value };

  static char ID;

  StaticSharingProbePass(
      std::string functionName, std::string symbolName, QueryKind queryKind,
      StaticThreadSharingAnalysis::SharingClassification *result)
      : ModulePass(ID), m_function_name(std::move(functionName)),
        m_symbol_name(std::move(symbolName)), m_query_kind(queryKind),
        m_result(result) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<StaticThreadSharingAnalysis>();
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override {
    if (!m_result) {
      return false;
    }

    const Function *F = M.getFunction(m_function_name);
    if (!F) {
      return false;
    }

    const Instruction *target = findInstructionByName(*F, m_symbol_name);
    if (!target) {
      return false;
    }

    auto &sharing = getAnalysis<StaticThreadSharingAnalysis>();
    if (m_query_kind == QueryKind::Instruction) {
      *m_result = sharing.classify(target);
    } else {
      *m_result = sharing.classify(static_cast<const Value *>(target));
    }
    return false;
  }

private:
  std::string m_function_name;
  std::string m_symbol_name;
  QueryKind m_query_kind;
  StaticThreadSharingAnalysis::SharingClassification *m_result;
};

char StaticSharingProbePass::ID = 0;

class StaticThreadSharingAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;

  static void ensurePassesInitialized() {
    static bool initialized = false;
    if (initialized) {
      return;
    }
    PassRegistry &registry = *PassRegistry::getPassRegistry();
    seadsa::initializeAnalysisPasses(registry);
    llvm::initializeDsaAnalysisPass(registry);
    initialized = true;
  }

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("StaticThreadSharingAnalysisTest", errs());
    }
    return module;
  }
};

TEST_F(StaticThreadSharingAnalysisTest,
       ClassifyInstructionHandlesGlobalOnlyNodes) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 1, i32* @g, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      %main_load = load i32, i32* @g, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "main", "main_load", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(
      observed,
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       SingleSpawnWorkerWriteDoesNotForceMultiRunSharing) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %local_slot = alloca i32, align 4
      store i32 7, i32* %local_slot, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass("worker", "local_slot",
                                    StaticSharingProbePass::QueryKind::Value,
                                    &observed));
  PM.run(*module);

  EXPECT_EQ(observed, StaticThreadSharingAnalysis::SharingClassification::
                          DefinitelyThreadLocal);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
