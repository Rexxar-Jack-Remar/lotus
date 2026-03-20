#include <gtest/gtest.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/SourceMgr.h>

#define private public
#define protected public
#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/LotusAA/Support/Config.h"
#undef protected
#undef private

using namespace llvm;

namespace {

std::unique_ptr<Module> parseAssembly(LLVMContext &Ctx, const char *IR) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("LotusAATest", errs());
  return M;
}

LotusAA *runLotusAA(Module &M) {
  auto *PM = new legacy::PassManager();
  auto *Pass = new LotusAA();
  PM->add(Pass);
  PM->run(M);
  return Pass;
}

void computeAllFunctionCgs(Module &M, LotusAA &Pass) {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (auto *PTG = Pass.getPtGraph(&F))
      PTG->computeCG();
  }
}

std::vector<CallBase *> getIndirectCalls(Function &F) {
  std::vector<CallBase *> Calls;
  for (Instruction &I : instructions(F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (CB->isIndirectCall())
        Calls.push_back(CB);
    }
  }
  return Calls;
}

Value *findValueByName(Function &F, StringRef Name) {
  for (Argument &Arg : F.args()) {
    if (Arg.getName() == Name)
      return &Arg;
  }
  for (Instruction &I : instructions(F)) {
    if (I.getName() == Name)
      return &I;
  }
  return nullptr;
}

bool containsValueAtom(path_cond_t cond, Value *value, bool sense) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::ValueAtom ||
      cond->getKind() == PathCond::Kind::BranchAtom) {
    return cond->getValue() == value && cond->getSense() == sense;
  }

  return containsValueAtom(cond->getLhs(), value, sense) ||
         containsValueAtom(cond->getRhs(), value, sense);
}

bool containsImportedAtom(path_cond_t cond, Value *callsite, Function *callee) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::ImportedAtom) {
    return cond->getValue() == callsite && cond->getCallee() == callee;
  }

  return containsImportedAtom(cond->getLhs(), callsite, callee) ||
         containsImportedAtom(cond->getRhs(), callsite, callee);
}

bool containsSwitchCaseAtom(path_cond_t cond, Value *value,
                            const APInt &case_value) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::SwitchCaseAtom) {
    ConstantInt *CI = cond->getCaseValue();
    return cond->getValue() == value && CI &&
           CI->getValue() == case_value;
  }

  return containsSwitchCaseAtom(cond->getLhs(), value, case_value) ||
         containsSwitchCaseAtom(cond->getRhs(), value, case_value);
}

bool containsSwitchDefaultAtom(path_cond_t cond, Value *value) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::SwitchDefaultAtom) {
    return cond->getValue() == value;
  }

  return containsSwitchDefaultAtom(cond->getLhs(), value) ||
         containsSwitchDefaultAtom(cond->getRhs(), value);
}

bool containsSummaryValue(const mem_value_t &values) {
  for (const auto &item : values) {
    if (item.val == LocValue::SUMMARY_VALUE)
      return true;
  }
  return false;
}

IntraLotusAA::OutputItem *findOutputItem(IntraLotusAA *PTG, Value *parent,
                                         int64_t offset) {
  for (auto *output : PTG->outputs) {
    auto &info = output->getSymbolicInfo();
    if (info.getParentPtr() == parent && info.getOffset() == offset)
      return output;
  }
  return nullptr;
}

struct LotusConfigScope {
  int inline_depth = IntraLotusAAConfig::lotus_restrict_inline_depth;
  int summary_ap_depth = IntraLotusAAConfig::lotus_restrict_summary_ap_depth;
  int inline_size = IntraLotusAAConfig::lotus_restrict_inline_size;
  int ap_level = IntraLotusAAConfig::lotus_restrict_ap_level;
  int cg_size = IntraLotusAAConfig::lotus_restrict_cg_size;
  bool use_full_phi_cond = IntraLotusAAConfig::lotus_use_full_phi_cond;
  bool enable_score_computation =
      IntraLotusAAConfig::lotus_enable_score_computation;
  bool enable_summary_value = IntraLotusAAConfig::lotus_enable_summary_value;
  int restrict_output_pts = IntraLotusAAConfig::lotus_restrict_output_pts;
  int max_passing_func = IntraLotusAAConfig::lotus_memory_max_passing_func;

  ~LotusConfigScope() {
    IntraLotusAAConfig::lotus_restrict_inline_depth = inline_depth;
    IntraLotusAAConfig::lotus_restrict_summary_ap_depth = summary_ap_depth;
    IntraLotusAAConfig::lotus_restrict_inline_size = inline_size;
    IntraLotusAAConfig::lotus_restrict_ap_level = ap_level;
    IntraLotusAAConfig::lotus_restrict_cg_size = cg_size;
    IntraLotusAAConfig::lotus_use_full_phi_cond = use_full_phi_cond;
    IntraLotusAAConfig::lotus_enable_score_computation =
        enable_score_computation;
    IntraLotusAAConfig::lotus_enable_summary_value = enable_summary_value;
    IntraLotusAAConfig::lotus_restrict_output_pts = restrict_output_pts;
    IntraLotusAAConfig::lotus_memory_max_passing_func = max_passing_func;
  }
};

} // namespace

TEST(LotusAA, PhiMergeResolvesBothIndirectTargets) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %sub = sub i32 %x, 1
      ret i32 %sub
    }

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @bar, %else ]
      %res = call i32 %fp(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 2u);
  EXPECT_TRUE(Targets.count(M->getFunction("foo")));
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, StructFieldGepKeepsNonZeroOffsetPrecision) {
  const char *IR = R"(
    %struct.S = type { i32 (i32)*, i32 (i32)* }

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %add = add i32 %x, 42
      ret i32 %add
    }

    define i32 @main() {
    entry:
      %s = alloca %struct.S
      %f0 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 1
      store i32 (i32)* @foo, i32 (i32)** %f0
      store i32 (i32)* @bar, i32 (i32)** %f1
      %loaded = load i32 (i32)*, i32 (i32)** %f1
      %res = call i32 %loaded(i32 1)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 1u);
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, PrunedInterfaceSpillsIntoSummaryBuckets) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;
  IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
  IntraLotusAAConfig::lotus_restrict_inline_size = 100;
  IntraLotusAAConfig::lotus_restrict_ap_level = 0;

  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    define void @set_cb(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i32 (i32)* %cb, i32 (i32)** %f
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("set_cb"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_EQ(PTG->outputs.size(), 1u);
  ASSERT_GE(PTG->summary_outputs.size(), 2u);
  EXPECT_FALSE(PTG->summary_outputs[1]->empty());
}

TEST(LotusAA, WeakUpdateKeepsComplementaryPathConditions) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b) {
    entry:
      %slot = alloca i8*
      store i8* %a, i8** %slot
      br i1 %cond, label %then, label %merge
    then:
      store i8* %b, i8** %slot
      br label %merge
    merge:
      %loaded = load i8*, i8** %slot
      ret i8* %loaded
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);

  mem_value_t LoadedValues;
  PTG->getLoadValues(Load->getPointerOperand(), Load, LoadedValues);
  PTG->refineResult(LoadedValues);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  for (const auto &Item : LoadedValues) {
    if (Item.val == findValueByName(*Main, "a"))
      cond_a = Item.cond;
    else if (Item.val == findValueByName(*Main, "b"))
      cond_b = Item.cond;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(cond_a));
  EXPECT_TRUE(PTG->isSatisfiable(cond_b));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_a));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_b));
  EXPECT_NE(cond_a, cond_b);
  Value *Cond = findValueByName(*Main, "cond");
  ASSERT_NE(Cond, nullptr);
  EXPECT_TRUE(containsValueAtom(cond_a, Cond, false));
  EXPECT_TRUE(containsValueAtom(cond_b, Cond, true));
}

TEST(LotusAA, PhiMergingSamePointerKeepsBothSiblingPathContributions) {
  const char *IR = R"(
    define i8* @main(i1 %cond) {
    entry:
      %x = alloca i8
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %x, %then ], [ %x, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*Main, "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  PTResultIterator iter(PhiPts, PTG);
  ASSERT_EQ(iter.size(), 1);
  path_cond_t cond = iter.begin()->second;
  EXPECT_TRUE(PTG->isAlwaysSatisfied(cond));
}

TEST(LotusAA, ReturnOutputsPreserveConditionalReturnSensitivity) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  for (const auto &ret_item : PTG->outputs.front()->getVal()) {
    for (const auto &mem_item : ret_item.second) {
      if (mem_item.val == findValueByName(*Main, "a"))
        cond_a = mem_item.cond;
      else if (mem_item.val == findValueByName(*Main, "b"))
        cond_b = mem_item.cond;
    }
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(cond_a));
  EXPECT_TRUE(PTG->isSatisfiable(cond_b));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_a));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_b));
  EXPECT_NE(cond_a, cond_b);

  Value *Cond = findValueByName(*Main, "cond");
  ASSERT_NE(Cond, nullptr);
  EXPECT_FALSE(PTG->isSatisfiable(
      PTG->findOrCreateAndRegion(cond_a, PTG->getValueCond(Cond, false))));
  EXPECT_FALSE(PTG->isSatisfiable(
      PTG->findOrCreateAndRegion(cond_b, PTG->getValueCond(Cond, true))));
}

TEST(LotusAA, PhiConditionsRetainNestedBranchPredicates) {
  const char *IR = R"(
    define i8* @main(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    right:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %b, %else ], [ %c, %right ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*Main, "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  Value *Outer = findValueByName(*Main, "outer");
  Value *Inner = findValueByName(*Main, "inner");
  ASSERT_NE(Outer, nullptr);
  ASSERT_NE(Inner, nullptr);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  path_cond_t cond_c = nullptr;

  PTResultIterator iter(PhiPts, PTG);
  for (auto &pt_item : iter) {
    Value *alloc_site = pt_item.first->getObj()->getAllocSite();
    if (alloc_site == findValueByName(*Main, "a"))
      cond_a = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "b"))
      cond_b = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "c"))
      cond_c = pt_item.second;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  ASSERT_NE(cond_c, nullptr);

  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_a));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_b));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_c));

  EXPECT_TRUE(containsValueAtom(cond_a, Outer, true));
  EXPECT_TRUE(containsValueAtom(cond_a, Inner, true));
  EXPECT_TRUE(containsValueAtom(cond_b, Outer, true));
  EXPECT_TRUE(containsValueAtom(cond_b, Inner, false));
  EXPECT_TRUE(containsValueAtom(cond_c, Outer, false));
}

TEST(LotusAA, InterproceduralSideEffectWritesKeepCallerFunctionLevel) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 1;
  IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
  IntraLotusAAConfig::lotus_restrict_inline_size = 100;
  IntraLotusAAConfig::lotus_restrict_ap_level = 1;

  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    define void @set_cb(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i32 (i32)* %cb, i32 (i32)** %f
      ret void
    }

    define void @wrapper(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      call void @set_cb(%struct.S* %s, i32 (i32)* %cb)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Pass->computePTA(M->getFunction("set_cb"));
  Pass->computePTA(M->getFunction("wrapper"));
  auto *PTG = Pass->getPtGraph(M->getFunction("wrapper"));
  ASSERT_NE(PTG, nullptr);

  auto *Wrapper = M->getFunction("wrapper");
  ASSERT_NE(Wrapper, nullptr);
  auto *ArgS = dyn_cast<Argument>(findValueByName(*Wrapper, "s"));
  ASSERT_NE(ArgS, nullptr);

  PTResult *ArgPts = PTG->findPTResult(ArgS, false);
  ASSERT_NE(ArgPts, nullptr);
  PTResultIterator iter(ArgPts, PTG);
  ASSERT_EQ(iter.size(), 1);

  ObjectLocator *field_loc = iter.begin()->first->getObj()->findLocator(0, true);
  ASSERT_NE(field_loc, nullptr);
  EXPECT_EQ(field_loc->getStoreFunctionLevel(), 1);
}

TEST(LotusAA, UnknownLibraryCallPreservesLikelyThisReceiver) {
  const char *IR = R"(
    %struct.S = type { i8* }

    declare void @ext(%struct.S*, i8**)

    define i8* @main(i8* %a, i8* %b) {
    entry:
      %s = alloca %struct.S
      %slot = alloca i8*
      %field = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i8* %a, i8** %field
      store i8* %b, i8** %slot
      call void @ext(%struct.S* %s, i8** %slot)
      %keep = load i8*, i8** %field
      %clobbered = load i8*, i8** %slot
      ret i8* %keep
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *KeepLoad = dyn_cast<LoadInst>(findValueByName(*Main, "keep"));
  auto *ClobLoad = dyn_cast<LoadInst>(findValueByName(*Main, "clobbered"));
  ASSERT_NE(KeepLoad, nullptr);
  ASSERT_NE(ClobLoad, nullptr);

  mem_value_t keep_values;
  PTG->getLoadValues(KeepLoad->getPointerOperand(), KeepLoad, keep_values);
  PTG->refineResult(keep_values);

  bool saw_a = false;
  for (const auto &item : keep_values) {
    if (item.val == findValueByName(*Main, "a"))
      saw_a = true;
  }
  EXPECT_TRUE(saw_a);

  mem_value_t clobbered_values;
  PTG->getLoadValues(ClobLoad->getPointerOperand(), ClobLoad, clobbered_values);
  PTG->refineResult(clobbered_values);

  bool saw_b = false;
  for (const auto &item : clobbered_values) {
    if (item.val == findValueByName(*Main, "b"))
      saw_b = true;
  }
  EXPECT_FALSE(saw_b);
}

TEST(LotusAA, IncompatibleIndirectTargetsDoNotConsumeCalleeIndexTwice) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_cg_size = 3;

  const char *IR = R"(
    define void @bad1(i64 %x) {
    entry:
      ret void
    }

    define void @bad2(i1 %x) {
    entry:
      ret void
    }

    define i8* @good(i8* %x) {
    entry:
      ret i8* %x
    }

    define i8* @main(i8* %arg, i8* (i8*)* %fp) {
    entry:
      %res = call i8* %fp(i8* %arg)
      ret i8* %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Good = M->getFunction("good");
  Function *Bad1 = M->getFunction("bad1");
  Function *Bad2 = M->getFunction("bad2");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Good, nullptr);
  ASSERT_NE(Bad1, nullptr);
  ASSERT_NE(Bad2, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);
  CallBase *Call = Calls.front();

  CallTargetSet targets;
  targets[Bad1] = nullptr;
  targets[Bad2] = nullptr;
  targets[Good] = nullptr;
  Pass->getFunctionPointerResults().setTargets(Main, Call, targets);

  Pass->computePTA(Good);
  Pass->computePTA(Main);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  ASSERT_TRUE(PTG->func_arg.count(Call));
  EXPECT_TRUE(PTG->func_arg[Call].count(Good));
}

TEST(LotusAA, FullyInlineInterfaceCapsDepthAtFalconMaximum) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;
  IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
  IntraLotusAAConfig::lotus_restrict_inline_size = 100;
  IntraLotusAAConfig::lotus_restrict_ap_level = 2;

  const char *IR = R"(
    define void @noop(i8* %p) {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("noop"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_EQ(PTG->getInlineApDepth(), ::LotusConfig::MAXIMAL_SUMMARY_AP_DEPTH);
}

TEST(LotusAA, SwitchConditionsPreserveCaseSensitivity) {
  const char *IR = R"(
    define i8* @main(i32 %tag, i8* %a, i8* %b, i8* %c) {
    entry:
      switch i32 %tag, label %default [
        i32 1, label %one
        i32 2, label %two
      ]
    one:
      br label %merge
    two:
      br label %merge
    default:
      br label %merge
    merge:
      %p = phi i8* [ %a, %one ], [ %b, %two ], [ %c, %default ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*Main, "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  Value *Tag = findValueByName(*Main, "tag");
  ASSERT_NE(Tag, nullptr);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  path_cond_t cond_c = nullptr;

  PTResultIterator iter(PhiPts, PTG);
  for (auto &pt_item : iter) {
    Value *alloc_site = pt_item.first->getObj()->getAllocSite();
    if (alloc_site == findValueByName(*Main, "a"))
      cond_a = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "b"))
      cond_b = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "c"))
      cond_c = pt_item.second;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  ASSERT_NE(cond_c, nullptr);
  EXPECT_TRUE(containsSwitchCaseAtom(cond_a, Tag, APInt(32, 1)));
  EXPECT_TRUE(containsSwitchCaseAtom(cond_b, Tag, APInt(32, 2)));
  EXPECT_TRUE(containsSwitchDefaultAtom(cond_c, Tag));
}

TEST(LotusAA, ImportedSummaryGuardsStayCallerLocal) {
  const char *IR = R"(
    define i8* @choose(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    }

    define i8* @main(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %outer, label %then, label %else
    then:
      %call = call i8* @choose(i1 %inner, i8* %a, i8* %b)
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %call, %then ], [ %c, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Choose = M->getFunction("choose");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Choose, nullptr);

  auto *MainPTG = Pass->getPtGraph(Main);
  auto *ChoosePTG = Pass->getPtGraph(Choose);
  ASSERT_NE(MainPTG, nullptr);
  ASSERT_NE(ChoosePTG, nullptr);

  auto *Call = dyn_cast<CallBase>(findValueByName(*Main, "call"));
  ASSERT_NE(Call, nullptr);

  path_cond_t callee_cond_a = nullptr;
  path_cond_t callee_cond_b = nullptr;
  for (const auto &ret_pair : ChoosePTG->outputs.front()->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == findValueByName(*Choose, "a"))
        callee_cond_a = item.cond;
      else if (item.val == findValueByName(*Choose, "b"))
        callee_cond_b = item.cond;
    }
  }

  ASSERT_NE(callee_cond_a, nullptr);
  ASSERT_NE(callee_cond_b, nullptr);
  path_cond_t cond_a = MainPTG->importPathCond(callee_cond_a, Call, Choose);
  path_cond_t cond_b = MainPTG->importPathCond(callee_cond_b, Call, Choose);
  EXPECT_TRUE(containsImportedAtom(cond_a, Call, Choose));
  EXPECT_TRUE(containsImportedAtom(cond_b, Call, Choose));
  Value *Inner = findValueByName(*Main, "inner");
  ASSERT_NE(Inner, nullptr);
  EXPECT_FALSE(containsValueAtom(cond_a, Inner, true));
  EXPECT_FALSE(containsValueAtom(cond_b, Inner, false));
}

TEST(LotusAA, DefaultSummaryCollectionDoesNotEmitSummaryValue) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_enable_score_computation = false;
  IntraLotusAAConfig::lotus_enable_summary_value = false;

  const char *IR = R"(
    define void @foo(i8** %p, i8* %a, i1 %cond) {
    entry:
      store i8* %a, i8** %p
      br i1 %cond, label %recurse, label %exit
    recurse:
      call void @foo(i8** %p, i8* %a, i1 false)
      br label %exit
    exit:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Foo = M->getFunction("foo");
  auto *PTG = Pass->getPtGraph(Foo);
  ASSERT_NE(PTG, nullptr);
  auto *Output = findOutputItem(PTG, &*Foo->arg_begin(), 0);
  ASSERT_NE(Output, nullptr);

  bool saw_a = false;
  bool saw_summary = false;
  for (auto &ret_pair : Output->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == findValueByName(*Foo, "a"))
        saw_a = true;
    }
    saw_summary |= containsSummaryValue(ret_pair.second);
  }

  EXPECT_TRUE(saw_a);
  EXPECT_FALSE(saw_summary);
}

TEST(LotusAA, EnabledSummaryCollectionEmitsSummaryValue) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_enable_score_computation = true;
  IntraLotusAAConfig::lotus_enable_summary_value = true;

  const char *IR = R"(
    define void @foo(i8** %p, i8* %a, i1 %cond) {
    entry:
      store i8* %a, i8** %p
      br i1 %cond, label %recurse, label %exit
    recurse:
      call void @foo(i8** %p, i8* %a, i1 false)
      br label %exit
    exit:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Foo = M->getFunction("foo");
  auto *PTG = Pass->getPtGraph(Foo);
  ASSERT_NE(PTG, nullptr);
  auto *Output = findOutputItem(PTG, &*Foo->arg_begin(), 0);
  ASSERT_NE(Output, nullptr);

  bool saw_summary = false;
  bool saw_fractional_confidence = false;
  for (auto &ret_pair : Output->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == LocValue::SUMMARY_VALUE) {
        saw_summary = true;
        if (item.confidence < 1.0f)
          saw_fractional_confidence = true;
      }
    }
  }

  EXPECT_TRUE(saw_summary);
  EXPECT_TRUE(saw_fractional_confidence);
}

TEST(LotusAA, FullyAnalyzedCalleeDoesNotAddSummaryValueNoise) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_enable_score_computation = true;
  IntraLotusAAConfig::lotus_enable_summary_value = true;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;

  const char *IR = R"(
    define void @setp(i8** %p, i8* %x) {
    entry:
      store i8* %x, i8** %p
      ret void
    }

    define void @wrapper(i8** %p, i8* %a, i8* %b) {
    entry:
      store i8* %a, i8** %p
      call void @setp(i8** %p, i8* %b)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Wrapper = M->getFunction("wrapper");
  auto *PTG = Pass->getPtGraph(Wrapper);
  ASSERT_NE(PTG, nullptr);
  auto *Output = findOutputItem(PTG, &*Wrapper->arg_begin(), 0);
  ASSERT_NE(Output, nullptr);

  bool saw_summary = false;
  for (auto &ret_pair : Output->getVal()) {
    saw_summary |= containsSummaryValue(ret_pair.second);
  }

  EXPECT_FALSE(saw_summary);
}

TEST(LotusAA, OutputPseudoPointsToMergesDuplicateEntries) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %a, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  auto &pseudo_pts = PTG->outputs.front()->getPseudoPointTo();
  ASSERT_EQ(pseudo_pts.size(), 1u);
  EXPECT_TRUE(PTG->isAlwaysSatisfied(pseudo_pts.front().first));
}

TEST(LotusAA, OutputPseudoPointsToRespectsConfiguredLimit) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_output_pts = 1;

  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %b, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  EXPECT_TRUE(PTG->outputs.front()->getPseudoPointTo().empty());
}

#ifndef NDEBUG
TEST(LotusAA, ReassigningSSAValueTriggersAssertion) {
  const char *IR = R"(
    define void @main() {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_DEATH(
      {
        Argument *Synthetic = new Argument(PTGraph::DEFAULT_POINTER_TYPE);
        MemObject *Obj = PTG->newObject(Synthetic, MemObject::CONCRETE);
        PTG->addPointsTo(Synthetic, Obj, 0, PTG->getEmptyCond());
        PTG->addPointsTo(Synthetic, Obj, 0, PTG->getEmptyCond());
      },
      "Re-assigning value");
}
#endif
