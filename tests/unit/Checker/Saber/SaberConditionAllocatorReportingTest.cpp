#include "SaberConditionAllocatorTestSupport.h"

TEST(SaberConditionAllocatorTest,
     DefaultModeDetectsStackMediatedDoubleFree) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i32 @main() {
    entry:
      %slot = alloca i8*
      %p = call i8* @malloc(i64 4)
      store i8* %p, i8** %slot
      %q = load i8*, i8** %slot
      call void @free(i8* %q)
      %r = load i8*, i8** %slot
      call void @free(i8* %r)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Double Free");

  DoubleFreeChecker checker;
  checker.runOnModule(*module);

  EXPECT_EQ(getReportCountForType(mgr, "Double Free"), reportsBefore + 1);
}
TEST(SaberConditionAllocatorTest, FileCheckerReportsFopenCallsiteAsSourceStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    %struct._IO_FILE = type opaque
    @path = private unnamed_addr constant [9 x i8] c"test.txt\00"
    @mode = private unnamed_addr constant [2 x i8] c"r\00"

    declare %struct._IO_FILE* @fopen(i8*, i8*)
    declare i32 @fclose(%struct._IO_FILE*)

    define void @cleanup() {
    entry:
      call i32 @fclose(%struct._IO_FILE* null)
      ret void
    }

    define void @leak_file() {
    entry:
      %path.ptr = getelementptr inbounds [9 x i8], [9 x i8]* @path, i64 0, i64 0
      %mode.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @mode, i64 0, i64 0
      %fp = call %struct._IO_FILE* @fopen(i8* %path.ptr, i8* %mode.ptr)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak_file()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "File Descriptor Leak");

  FileChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "File Descriptor Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "File Descriptor Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());

  const BugDiagStep *sourceStep = report->get_steps().front();
  ASSERT_NE(sourceStep, nullptr);
  EXPECT_EQ(sourceStep->tip, "File opened here");
  const auto *call = dyn_cast_or_null<CallBase>(sourceStep->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "fopen");
  EXPECT_FALSE(reportHasStepTip(report, "File closed here"));
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerPartialLeakReportDoesNotAttributeFreeStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @maybe_leak(i1 %cond) {
    entry:
      %p = call i8* @malloc(i64 4)
      br i1 %cond, label %freebb, label %leakbb

    freebb:
      call void @free(i8* %p)
      ret void

    leakbb:
      ret void
    }

    define i32 @main(i32 %argc, i8** %argv) {
    entry:
      %cond = icmp eq i32 %argc, 0
      call void @maybe_leak(i1 %cond)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak 2");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak 2");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Memory Leak 2");
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportHasStepTip(report, "Path condition"));
  EXPECT_FALSE(reportHasStepTip(report, "Memory deallocated here"));
}
TEST(SaberConditionAllocatorTest,
     FileCheckerPartialLeakReportDoesNotAttributeCloseStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    %struct._IO_FILE = type opaque
    @path = private unnamed_addr constant [9 x i8] c"test.txt\00"
    @mode = private unnamed_addr constant [2 x i8] c"r\00"

    declare %struct._IO_FILE* @fopen(i8*, i8*)
    declare i32 @fclose(%struct._IO_FILE*)

    define void @maybe_close(i1 %cond) {
    entry:
      %path.ptr = getelementptr inbounds [9 x i8], [9 x i8]* @path, i64 0, i64 0
      %mode.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @mode, i64 0, i64 0
      %fp = call %struct._IO_FILE* @fopen(i8* %path.ptr, i8* %mode.ptr)
      br i1 %cond, label %closebb, label %leakbb

    closebb:
      call i32 @fclose(%struct._IO_FILE* %fp)
      ret void

    leakbb:
      ret void
    }

    define i32 @main(i32 %argc, i8** %argv) {
    entry:
      %cond = icmp eq i32 %argc, 0
      call void @maybe_close(i1 %cond)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "File Descriptor Leak 2");

  FileChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "File Descriptor Leak 2");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "File Descriptor Leak 2");
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportHasStepTip(report, "Path condition"));
  EXPECT_FALSE(reportHasStepTip(report, "File closed here"));
}
TEST(SaberConditionAllocatorTest,
     DefaultModeHandlesStackMediatedFopenFcloseWithoutFalseLeak) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    %struct._IO_FILE = type opaque
    @path = private unnamed_addr constant [2 x i8] c"x\00"
    @mode = private unnamed_addr constant [2 x i8] c"r\00"

    declare %struct._IO_FILE* @fopen(i8*, i8*)
    declare i32 @fclose(%struct._IO_FILE*)

    define i32 @main() {
    entry:
      %slot = alloca %struct._IO_FILE*
      %path.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @path, i64 0, i64 0
      %mode.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @mode, i64 0, i64 0
      %fp = call %struct._IO_FILE* @fopen(i8* %path.ptr, i8* %mode.ptr)
      store %struct._IO_FILE* %fp, %struct._IO_FILE** %slot
      %loaded = load %struct._IO_FILE*, %struct._IO_FILE** %slot
      call i32 @fclose(%struct._IO_FILE* %loaded)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, "File Descriptor Leak");

  FileChecker checker;
  checker.runOnModule(*module);

  EXPECT_EQ(getReportCountForType(mgr, "File Descriptor Leak"), reportsBefore);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerDetectsBitcastedDirectAllocatorCalls) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @cleanup() {
    entry:
      call void @free(i8* null)
      ret void
    }

    define void @leak() {
    entry:
      %p = call i8* (i64, ...) bitcast (i8* (i64)* @malloc to i8* (i64, ...)*)(i64 4)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}
TEST(SaberConditionAllocatorTest,
     DoubleFreeCheckerDetectsBitcastedDirectDeallocatorCalls) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @df() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void (i8*, ...) bitcast (void (i8*)* @free to void (i8*, ...)*)(i8* %p)
      call void (i8*, ...) bitcast (void (i8*)* @free to void (i8*, ...)*)(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      call void @df()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Double Free");

  DoubleFreeChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Double Free");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}
TEST(SaberConditionAllocatorTest,
     NonFullModeRetainsDistinctActualParmSinksForRepeatedFrees) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @df() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      call void @df()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  SaberOptionScope optionScope;
  SaberFullSVFG = false;

  LeakChecker checker;
  checker.setModule(module.get());
  checker.initialize();

  const SVFG *svfg = checker.getSVFG();
  ASSERT_NE(svfg, nullptr);

  auto frees = getCallsTo(*module, "df", "free");
  ASSERT_EQ(frees.size(), 2u);

  const auto &firstParms = svfg->getActualParms(frees[0]);
  const auto &secondParms = svfg->getActualParms(frees[1]);
  ASSERT_EQ(firstParms.size(), 1u);
  ASSERT_EQ(secondParms.size(), 1u);
  EXPECT_NE((*firstParms.begin())->getId(), (*secondParms.begin())->getId());
}
TEST(SaberConditionAllocatorTest,
     NonFullModeRetainsDistinctActualParmSinksForBitcastedFrees) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @df() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void (i8*, ...) bitcast (void (i8*)* @free to void (i8*, ...)*)(i8* %p)
      call void (i8*, ...) bitcast (void (i8*)* @free to void (i8*, ...)*)(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      call void @df()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  SaberOptionScope optionScope;
  SaberFullSVFG = false;

  LeakChecker checker;
  checker.setModule(module.get());
  checker.initialize();

  const SVFG *svfg = checker.getSVFG();
  ASSERT_NE(svfg, nullptr);

  auto frees = getCallsTo(*module, "df", "free");
  ASSERT_EQ(frees.size(), 2u);

  const auto &firstParms = svfg->getActualParms(frees[0]);
  const auto &secondParms = svfg->getActualParms(frees[1]);
  ASSERT_EQ(firstParms.size(), 1u);
  ASSERT_EQ(secondParms.size(), 1u);
  EXPECT_NE((*firstParms.begin())->getId(), (*secondParms.begin())->getId());
}
TEST(SaberConditionAllocatorTest,
     FileCheckerDetectsBitcastedDirectFopenCalls) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    %struct._IO_FILE = type opaque
    @path = private unnamed_addr constant [9 x i8] c"test.txt\00"
    @mode = private unnamed_addr constant [2 x i8] c"r\00"

    declare %struct._IO_FILE* @fopen(i8*, i8*)
    declare i32 @fclose(%struct._IO_FILE*)

    define void @cleanup() {
    entry:
      call i32 @fclose(%struct._IO_FILE* null)
      ret void
    }

    define void @leak_file() {
    entry:
      %path.ptr = getelementptr inbounds [9 x i8], [9 x i8]* @path, i64 0, i64 0
      %mode.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @mode, i64 0, i64 0
      %fp = call %struct._IO_FILE* (i8*, i8*, ...) bitcast (%struct._IO_FILE* (i8*, i8*)* @fopen to %struct._IO_FILE* (i8*, i8*, ...)*)(i8* %path.ptr, i8* %mode.ptr)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak_file()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, "File Descriptor Leak");

  FileChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter =
      getReportCountForType(mgr, "File Descriptor Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}
TEST(SaberConditionAllocatorTest,
     NonFullModeRetainsDistinctActualParmSinksForRepeatedFcloses) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    %struct._IO_FILE = type opaque
    @path = private unnamed_addr constant [9 x i8] c"test.txt\00"
    @mode = private unnamed_addr constant [2 x i8] c"r\00"

    declare %struct._IO_FILE* @fopen(i8*, i8*)
    declare i32 @fclose(%struct._IO_FILE*)

    define void @close_twice() {
    entry:
      %path.ptr = getelementptr inbounds [9 x i8], [9 x i8]* @path, i64 0, i64 0
      %mode.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @mode, i64 0, i64 0
      %fp = call %struct._IO_FILE* @fopen(i8* %path.ptr, i8* %mode.ptr)
      %a = call i32 @fclose(%struct._IO_FILE* %fp)
      %b = call i32 @fclose(%struct._IO_FILE* %fp)
      ret void
    }

    define i32 @main() {
    entry:
      call void @close_twice()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  SaberOptionScope optionScope;
  SaberFullSVFG = false;

  FileChecker checker;
  checker.setModule(module.get());
  checker.initialize();

  const SVFG *svfg = checker.getSVFG();
  ASSERT_NE(svfg, nullptr);

  auto closes = getCallsTo(*module, "close_twice", "fclose");
  ASSERT_EQ(closes.size(), 2u);

  const auto &firstParms = svfg->getActualParms(closes[0]);
  const auto &secondParms = svfg->getActualParms(closes[1]);
  ASSERT_EQ(firstParms.size(), 1u);
  ASSERT_EQ(secondParms.size(), 1u);
  EXPECT_NE((*firstParms.begin())->getId(), (*secondParms.begin())->getId());
}
TEST(SaberConditionAllocatorTest,
     ContextOverflowStillTraversesWrapperCallsLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8* @alloc_wrapper() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret i8* %p
    }

    define void @free_wrapper(i8* %p) {
    entry:
      call void @free(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      %p = call i8* @alloc_wrapper()
      call void @free_wrapper(i8* %p)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  SaberOptionScope optionScope;
  SaberFullSVFG = true;
  SaberCxtLimit = 0;

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  EXPECT_EQ(getReportCountForType(mgr, "Memory Leak"), reportsBefore);
}
TEST(SaberConditionAllocatorTest,
     SaberFullSVFGOptionChangesConstructedGraphShape) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @alloc_wrapper() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret i8* %p
    }

    define i8* @use_wrapper() {
    entry:
      %p = call i8* @alloc_wrapper()
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %p = call i8* @use_wrapper()
      %isnull = icmp eq i8* %p, null
      %ret = zext i1 %isnull to i32
      ret i32 %ret
    }
  )");
  ASSERT_NE(module, nullptr);

  auto countInterprocShapeNodes = [](const SVFG *svfg) {
    size_t count = 0;
    for (const auto &entry : *svfg) {
      const SVFGNode *node = entry.second;
      if (isa<FormalParmSVFGNode>(node) || isa<FormalRetSVFGNode>(node) ||
          isa<ActualParmSVFGNode>(node) || isa<ActualRetSVFGNode>(node)) {
        ++count;
      }
    }
    return count;
  };

  size_t compatShapeNodes = 0;
  {
    SaberOptionScope optionScope;
    SaberFullSVFG = false;
    LeakChecker checker;
    checker.setModule(module.get());
    checker.initialize();
    ASSERT_NE(checker.getSVFG(), nullptr);
    compatShapeNodes = countInterprocShapeNodes(checker.getSVFG());
  }

  size_t fullShapeNodes = 0;
  {
    SaberOptionScope optionScope;
    SaberFullSVFG = true;
    LeakChecker checker;
    checker.setModule(module.get());
    checker.initialize();
    ASSERT_NE(checker.getSVFG(), nullptr);
    fullShapeNodes = countInterprocShapeNodes(checker.getSVFG());
  }

  EXPECT_LT(compatShapeNodes, fullShapeNodes);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerReportsAddressTakenButUnreachableHelpersLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    @fp = internal global i8* ()* @helper

    declare i8* @malloc(i64)
    declare void @free(i8*)

    define internal i8* @helper() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret i8* %p
    }

    define internal void @cleanup() {
    entry:
      call void @free(i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      call void @cleanup()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  SaberOptionScope optionScope;
  SaberFullSVFG = true;

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Memory Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());
  const auto *call = dyn_cast_or_null<CallBase>(report->get_steps().front()->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "malloc");
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerDoesNotSkipAvailableExternallyBodiesLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define available_externally void @helper() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret void
    }

    define i32 @main() {
    entry:
      call void @helper()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Memory Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());
  const auto *call = dyn_cast_or_null<CallBase>(report->get_steps().front()->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "malloc");
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerSkipsSummaryBackedFunctionsLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define void @memcpy(i8* %dst, i8* %src, i64 %n) {
    entry:
      %p = call i8* @malloc(i64 4)
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca i8, align 1
      call void @memcpy(i8* %buf, i8* %buf, i64 1)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getMemoryLeakReportCount(mgr);

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getMemoryLeakReportCount(mgr);
  EXPECT_EQ(reportsAfter, reportsBefore);
}
