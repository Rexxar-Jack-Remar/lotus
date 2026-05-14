#include "Checker/KINT/BugDetection.h"
#include "Checker/KINT/KINTTaintAnalysis.h"
#include "Checker/KINT/MKintPass.h"
#include "Checker/KINT/Options.h"
#include "Checker/KINT/SmtMemory.h"
#include "TestUtils/LLVMHelpers.h"

#include <optional>

#include <gtest/gtest.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <z3++.h>

using namespace llvm;

namespace {

uint64_t getNumeralU64(const z3::expr &expr) {
  z3::expr simplified = expr.simplify();
  uint64_t value = 0;
  EXPECT_TRUE(Z3_get_numeral_uint64(simplified.ctx(), simplified, &value));
  return value;
}

z3::expr bvValFromAPInt(z3::context &ctx, const llvm::APInt &value) {
  llvm::SmallString<64> decimal;
  value.toString(decimal, 10, /*Signed=*/false, /*formatAsCLiteral=*/false);
  Z3_sort sort = Z3_mk_bv_sort(ctx, value.getBitWidth());
  Z3_ast ast = Z3_mk_numeral(ctx, decimal.c_str(), sort);
  return z3::to_expr(ctx, ast);
}

class KINTCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    return lotus::unittest::parseModule(context, source, "KINTCheckerTest");
  }

  void runPass(Module &module) {
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    kint::MKintPass pass;
    pass.run(module, MAM);
  }

  static bool hasKintErrorMetadata(const Function &F,
                                   llvm::Instruction::BinaryOps opcode) {
    for (const Instruction &I : instructions(F)) {
      const auto *bin = dyn_cast<BinaryOperator>(&I);
      if (!bin || bin->getOpcode() != opcode)
        continue;
      if (bin->getMetadata("mkint.err"))
        return true;
    }
    return false;
  }
};

TEST_F(KINTCheckerTest, SmtMemoryLittleEndianRoundTrip) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(0, 64);

  memory.storeBytes(addr, ctx.bv_val(0x1234, 16), 2, true);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 2, true)), 0x1234u);
}

TEST_F(KINTCheckerTest, SmtMemoryBigEndianRoundTrip) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(0, 64);

  memory.storeBytes(addr, ctx.bv_val(0x1234, 16), 2, false);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 2, false)), 0x1234u);
}

TEST_F(KINTCheckerTest, GetSinkFnsHandlesIndirectCalls) {
  const char *source = R"(
    define void @indirect(void (i32)* %fp, i32 %x) {
    entry:
      %v = add i32 %x, 1
      call void %fp(i32 %v)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("indirect");
  ASSERT_NE(F, nullptr);

  auto it = inst_begin(*F);
  ASSERT_NE(it, inst_end(*F));
  auto *add = dyn_cast<BinaryOperator>(&*it);
  ASSERT_NE(add, nullptr);

  auto sinks = kint::TaintAnalysis::get_sink_fns(add);
  EXPECT_TRUE(sinks.empty());
}

TEST_F(KINTCheckerTest, IsSinkReachableTerminatesOnCycles) {
  const char *source = R"(
    @g = global i32 0

    define void @cycle(i32 %x) {
    entry:
      store i32 %x, i32* @g
      %v = load i32, i32* @g
      %inc = add i32 %v, 1
      store i32 %inc, i32* @g
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("cycle");
  ASSERT_NE(F, nullptr);

  StoreInst *first_store = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *store = dyn_cast<StoreInst>(&I)) {
      first_store = store;
      break;
    }
  }
  ASSERT_NE(first_store, nullptr);

  kint::TaintAnalysis analysis;
  SetVector<Function *> taint_funcs;
  EXPECT_FALSE(analysis.is_sink_reachable(first_store, taint_funcs));
  EXPECT_TRUE(taint_funcs.empty());
}

TEST_F(KINTCheckerTest, PropagateTaintAcrossFunctionsFindsTransitiveTaint) {
  const char *source = R"(
    declare void @__mkint_sink0(i32)

    define void @mid2(i32 %x) {
    entry:
      %v2 = add i32 %x, 1
      call void @__mkint_sink0(i32 %v2)
      ret void
    }

    define void @mid1(i32 %x) {
    entry:
      %v1 = add i32 %x, 1
      call void @mid2(i32 %v1)
      ret void
    }

    define void @__mkint_ann_source(i32 %x) {
    entry:
      call void @mid1(i32 %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::TaintAnalysis analysis;
  MapVector<Function *, std::vector<CallInst *>> func2tsrc;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;

  for (Function &F : *module) {
    auto taint_sources = analysis.get_taint_source(F);
    analysis.mark_func_sinks(F, callback_tsrc_fn);
    if (kint::TaintAnalysis::is_taint_src(F.getName())) {
      func2tsrc[&F] = std::move(taint_sources);
    }
  }

  analysis.propagate_taint_across_functions(*module, func2tsrc, taint_funcs);

  Function *mid1 = module->getFunction("mid1");
  Function *mid2 = module->getFunction("mid2");
  ASSERT_NE(mid1, nullptr);
  ASSERT_NE(mid2, nullptr);
  EXPECT_TRUE(taint_funcs.contains(mid1));
  EXPECT_TRUE(taint_funcs.contains(mid2));
}

TEST_F(KINTCheckerTest, ArgumentSinkUseIsRecognized) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @__mkint_ann_alloc_user(i64 %n) {
    entry:
      %p = call i8* @malloc(i64 %n)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::TaintAnalysis analysis;
  MapVector<Function *, std::vector<CallInst *>> func2tsrc;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;

  for (Function &F : *module) {
    auto taint_sources = analysis.get_taint_source(F);
    analysis.mark_func_sinks(F, callback_tsrc_fn);
    if (kint::TaintAnalysis::is_taint_src(F.getName())) {
      func2tsrc[&F] = std::move(taint_sources);
    }
  }

  analysis.propagate_taint_across_functions(*module, func2tsrc, taint_funcs);

  Function *malloc_fn = module->getFunction("malloc");
  ASSERT_NE(malloc_fn, nullptr);
  EXPECT_TRUE(taint_funcs.contains(malloc_fn));
}

TEST_F(KINTCheckerTest, BugDetectionKeepsDistinctBugTypesPerInstruction) {
  const char *source = R"(
    define i32 @div(i32 %x, i32 %y) {
    entry:
      %q = sdiv i32 %x, %y
      ret i32 %q
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("div");
  ASSERT_NE(F, nullptr);

  auto *div = dyn_cast<BinaryOperator>(&*inst_begin(*F));
  ASSERT_NE(div, nullptr);

  kint::BugDetection bug_detection;
  bug_detection.recordBug(div, kint::interr::INT_OVERFLOW);
  bug_detection.recordBug(div, kint::interr::DIV_BY_ZERO);

  EXPECT_EQ(bug_detection.getBugPaths().size(), 2u);
}

TEST_F(KINTCheckerTest, WideConstantPreservesHighBits) {
  const char *source = R"(
    define i128 @wide() {
    entry:
      ret i128 18446744073709551616
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("wide");
  ASSERT_NE(F, nullptr);
  auto *ret = dyn_cast<ReturnInst>(F->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);
  auto *ci = dyn_cast<ConstantInt>(ret->getReturnValue());
  ASSERT_NE(ci, nullptr);

  z3::context ctx;
  z3::solver solver(ctx);
  DenseMap<const Value *, std::optional<z3::expr>> empty;
  kint::BugDetection bug_detection;
  z3::expr actual = bug_detection.v2sym(ci, empty, solver);
  z3::expr expected = bvValFromAPInt(ctx, ci->getValue());

  solver.add(actual != expected);
  EXPECT_EQ(solver.check(), z3::unsat);
}

TEST_F(KINTCheckerTest,
       InterprocSummarySuppressesFalseDivZeroThroughPointerArg) {
  const char *source = R"(
    define void @set_one(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 0, i32* %x
      call void @set_one(i32* %x)
      %v = load i32, i32* %x
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummarySuppressesFalseDivZeroThroughGlobal) {
  const char *source = R"(
    @g = global i32 0

    define void @setg() {
    entry:
      store i32 1, i32* @g
      ret void
    }

    define i32 @main() {
    entry:
      call void @setg()
      %v = load i32, i32* @g
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryFramesUnmodifiedPointerArgument) {
  const char *source = R"(
    define void @touch_first(i32* %p, i32* %q) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      %y = alloca i32
      store i32 0, i32* %x
      store i32 1, i32* %y
      call void @touch_first(i32* %x, i32* %y)
      %v = load i32, i32* %y
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::On;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryPropagatesNestedPointerModification) {
  const char *source = R"(
    define void @leaf(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define void @mid(i32* %p) {
    entry:
      call void @leaf(i32* %p)
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 0, i32* %x
      call void @mid(i32* %x)
      %v = load i32, i32* %x
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummarySupportsPointerReturnFromArgument) {
  const char *source = R"(
    define i32* @idptr(i32* %p) {
    entry:
      ret i32* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 1, i32* %x
      %p = call i32* @idptr(i32* %x)
      %v = load i32, i32* %p
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummarySupportsPointerReturnFromGlobal) {
  const char *source = R"(
    @g = global i32 0

    define i32* @getg() {
    entry:
      ret i32* @g
    }

    define i32 @main() {
    entry:
      store i32 1, i32* @g
      %p = call i32* @getg()
      %v = load i32, i32* %p
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummarySupportsEscapedAllocatorInitialization) {
  const char *source = R"(
    declare i32* @malloc(i64)

    define i32* @alloc_init() {
    entry:
      %p = call i32* @malloc(i64 4)
      store i32 1, i32* %p
      ret i32* %p
    }

    define i32 @main() {
    entry:
      %p = call i32* @alloc_init()
      %v = load i32, i32* %p
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummaryFramesCallerHeapAcrossEscapedAllocator) {
  const char *source = R"(
    declare i32* @malloc(i64)

    define i32* @alloc_init() {
    entry:
      %p = call i32* @malloc(i64 4)
      store i32 1, i32* %p
      ret i32* %p
    }

    define i32 @main() {
    entry:
      %q = call i32* @malloc(i64 4)
      store i32 1, i32* %q
      %p = call i32* @alloc_init()
      %v = load i32, i32* %q
      %d = sdiv i32 42, %v
      ret i32 %d
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummaryExistentiallySeparatesHiddenPathWitnessesPerCall) {
  const char *source = R"(
    declare i32 @nondet() readnone

    define i32 @choose() {
    entry:
      %x = call i32 @nondet()
      %cmp = icmp eq i32 %x, 0
      br i1 %cmp, label %zero, label %one

    zero:
      ret i32 0

    one:
      ret i32 1
    }

    define i32 @main() {
    entry:
      %a = call i32 @choose()
      %b = call i32 @choose()
      %sum = add i32 %a, %b
      %is_one = icmp eq i32 %sum, 1
      br i1 %is_one, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::On;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryPrunesImpossibleCallerBranch) {
  const char *source = R"(
    define i32 @ret_zero() {
    entry:
      ret i32 0
    }

    define i32 @main() {
    entry:
      %v = call i32 @ret_zero()
      %is_one = icmp eq i32 %v, 1
      br i1 %is_one, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryHandlesPhiMergedReturnCases) {
  const char *source = R"(
    declare i32 @nondet() readnone

    define i32 @choose_phi() {
    entry:
      %x = call i32 @nondet()
      %cmp = icmp eq i32 %x, 0
      br i1 %cmp, label %zero, label %two

    zero:
      br label %merge

    two:
      br label %merge

    merge:
      %v = phi i32 [ 0, %zero ], [ 2, %two ]
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %v = call i32 @choose_phi()
      %is_one = icmp eq i32 %v, 1
      br i1 %is_one, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryTracksBooleanComposedPathCondition) {
  const char *source = R"(
    define i32 @range_flag(i32 %x) {
    entry:
      %gt_zero = icmp sgt i32 %x, 0
      %lt_ten = icmp slt i32 %x, 10
      %in_range = and i1 %gt_zero, %lt_ten
      br i1 %in_range, label %safe, label %bug

    safe:
      ret i32 1

    bug:
      ret i32 0
    }

    define i32 @main() {
    entry:
      %v = call i32 @range_flag(i32 5)
      %is_zero = icmp eq i32 %v, 0
      br i1 %is_zero, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryFallsBackOnAliasedActuals) {
  const char *source = R"(
    define void @write_both(i32* %p, i32* %q) {
    entry:
      store i32 1, i32* %p, align 4
      store i32 0, i32* %q, align 4
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32, align 4
      store i32 7, i32* %x, align 4
      call void @write_both(i32* %x, i32* %x)
      %v = load i32, i32* %x, align 4
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero.getValue();
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

} // namespace
