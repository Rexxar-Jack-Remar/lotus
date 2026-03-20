#include "Alias/TypeQualifier/Config.h"
#include "Alias/TypeQualifier/FunctionSummary.h"
#include "Alias/TypeQualifier/QualifierAnalysis.h"
#include "Alias/TypeQualifier/QualifierTypes.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

namespace {

std::unique_ptr<llvm::Module> parseAssembly(llvm::LLVMContext &ctx,
                                            const char *ir) {
  llvm::SMDiagnostic err;
  auto module = llvm::parseAssemblyString(ir, err, ctx);
  if (!module)
    err.print("TypeQualifierTest", llvm::errs());
  return module;
}

const llvm::CallInst *findIndirectCall(const llvm::Function &F) {
  for (const llvm::BasicBlock &BB : F) {
    for (const llvm::Instruction &I : BB) {
      auto *CI = llvm::dyn_cast<llvm::CallInst>(&I);
      if (CI && !CI->getCalledFunction())
        return CI;
    }
  }
  return nullptr;
}

} // namespace

TEST(TypeQualifier, DomainJoinPrefersUninitializedOverUnknown) {
  EXPECT_EQ(QualifierDomain::join(QualifierState::Initialized,
                                  QualifierState::Unknown),
            QualifierState::Unknown);
  EXPECT_EQ(QualifierDomain::join(QualifierState::Unknown,
                                  QualifierState::Uninitialized),
            QualifierState::Uninitialized);
  EXPECT_EQ(QualifierDomain::legacyMin(QualifierState::Initialized,
                                       QualifierState::Uninitialized),
            QualifierState::Uninitialized);
}

TEST(TypeQualifier, RegistryClassifiesKnownModels) {
  EXPECT_EQ(FunctionModelRegistry::lookup("malloc").kind,
            FunctionModelKind::Allocator);
  EXPECT_EQ(FunctionModelRegistry::lookup("kzalloc").kind,
            FunctionModelKind::ZeroAllocator);
  EXPECT_EQ(FunctionModelRegistry::lookup("llvm.memcpy.p0i8.p0i8.i64").kind,
            FunctionModelKind::Copy);
  EXPECT_EQ(FunctionModelRegistry::lookup("llvm.dbg.value").kind,
            FunctionModelKind::Ignore);
  EXPECT_EQ(FunctionModelRegistry::lookup("printf").kind,
            FunctionModelKind::Passthrough);
}

TEST(TypeQualifier, RegistryInitializesLegacySetsOnce) {
  GlobalContext ctx;
  initializeFunctionModelSets(ctx);

  EXPECT_TRUE(ctx.functionModelsInitialized);
  EXPECT_TRUE(ctx.HeapAllocFuncs.count("malloc"));
  EXPECT_TRUE(ctx.ZeroMallocFuncs.count("kzalloc"));
  EXPECT_TRUE(ctx.CopyFuncs.count("memcpy"));
  EXPECT_TRUE(ctx.TransferFuncs.count("copy_to_user"));
  EXPECT_TRUE(ctx.InitFuncs.count("memset"));
  EXPECT_TRUE(ctx.OtherFuncs.count("llvm.dbg.value"));
  EXPECT_TRUE(ctx.ObjSizeFuncs.count("llvm.objectsize.i64.p0i8"));
}

TEST(TypeQualifier, SummaryAccessorsExposeTypedStates) {
  Summary summary;
  summary.noNodes = 2;
  summary.reqVec.resize(2);
  summary.updateVec.resize(2);

  summary.setRequiredState(1, QualifierState::Initialized);
  summary.setUpdatedState(0, QualifierState::Uninitialized);

  EXPECT_EQ(summary.requiredState(1), QualifierState::Initialized);
  EXPECT_EQ(summary.returnState(), QualifierState::Uninitialized);
}

TEST(TypeQualifier, FuncAnalysisRunsOnDirectModeledAllocator) {
  const char *ir = R"IR(
    declare i8* @malloc(i64)

    define i32 @main() {
      %raw = call i8* @malloc(i64 16)
      %ptr = bitcast i8* %raw to i32*
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  GlobalContext gc;
  FuncAnalysis analysis(module->getFunction("main"), &gc, false);
  EXPECT_FALSE(analysis.run());
}

TEST(TypeQualifier, FuncAnalysisRunsOnIndirectModeledCopyTargets) {
  const char *ir = R"IR(
    declare i8* @memcpy(i8*, i8*, i64)
    declare i8* @memmove(i8*, i8*, i64)

    define i32 @main(i1 %cond) {
    entry:
      %fp = select i1 %cond,
                   i8* (i8*, i8*, i64)* @memcpy,
                   i8* (i8*, i8*, i64)* @memmove
      %dst = alloca [8 x i8]
      %src = alloca [8 x i8]
      %dst0 = getelementptr inbounds [8 x i8], [8 x i8]* %dst, i32 0, i32 0
      %src0 = getelementptr inbounds [8 x i8], [8 x i8]* %src, i32 0, i32 0
      %call = call i8* %fp(i8* %dst0, i8* %src0, i64 8)
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const llvm::CallInst *call = findIndirectCall(*mainFn);
  ASSERT_NE(call, nullptr);

  GlobalContext gc;
  gc.Callees[const_cast<llvm::CallInst *>(call)].insert(
      module->getFunction("memcpy"));
  gc.Callees[const_cast<llvm::CallInst *>(call)].insert(
      module->getFunction("memmove"));

  FuncAnalysis analysis(mainFn, &gc, false);
  EXPECT_FALSE(analysis.run());
}
