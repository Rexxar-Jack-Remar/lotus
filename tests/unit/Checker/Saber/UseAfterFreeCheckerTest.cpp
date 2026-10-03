#include "Checker/Saber/UseAfterFreeChecker.h"
#include "Checker/Framework/BugReportMgr.h"
#include "IR/ICFG/ICFGBuilder.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>

namespace {
std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &context,
                                    const char *source) {
  llvm::SMDiagnostic diagnostic;
  return llvm::parseAssemblyString(source, diagnostic, context);
}

std::size_t reportCount() {
  BugReportMgr &manager = BugReportMgr::get_instance();
  int id = manager.find_bug_type("Use After Free");
  if (id < 0) return 0;
  const auto *reports = manager.get_reports_for_type(id);
  return reports ? reports->size() : 0;
}

TEST(SaberUseAfterFree, MatchesCrossFunctionReturnsWithoutMixingCallers) {
  llvm::LLVMContext context;
  auto module = parse(context, R"IR(
    declare void @free(i8*)
    define void @release(i8* %p) {
      call void @free(i8* %p)
      ret void
    }
    define i8 @caller(i8* %p) {
      call void @release(i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }
    define i8 @unrelated(i8* %p) {
      %v = load i8, i8* %p
      ret i8 %v
    }
  )IR");
  ASSERT_TRUE(module);
  lotus::analysis::SVFG svfg;
  for (const char *name : {"release", "caller", "unrelated"})
    svfg.addObjectForValue(module->getFunction(name)->getArg(0), 7);
  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());
  auto before = reportCount();
  lotus::analysis::UseAfterFreeChecker().runOnModule(*module, svfg, icfg);
  EXPECT_EQ(reportCount(), before + 1);
}

TEST(SaberUseAfterFree, RespectsDisjointObjectsAndTemporalOrder) {
  llvm::LLVMContext context;
  auto module = parse(context, R"IR(
    declare void @free(i8*)
    define i8 @check(i8* %p, i8* %q) {
      %before = load i8, i8* %p
      call void @free(i8* %p)
      %after = load i8, i8* %q
      ret i8 %after
    }
  )IR");
  ASSERT_TRUE(module);
  lotus::analysis::SVFG svfg;
  auto *function = module->getFunction("check");
  svfg.addObjectForValue(function->getArg(0), 1);
  svfg.addObjectForValue(function->getArg(1), 2);
  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());
  auto before = reportCount();
  lotus::analysis::UseAfterFreeChecker().runOnModule(*module, svfg, icfg);
  EXPECT_EQ(reportCount(), before);
}
} // namespace
