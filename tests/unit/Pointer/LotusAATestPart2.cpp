#include "LotusAATestSupport.h"

TEST(LotusAA, PthreadCreateUnknownLibraryCallStillClobbersThreadArg) {
  const char *IR = R"(
    declare i32 @pthread_create(i64*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %ctx) {
    entry:
      ret i8* %ctx
    }

    define i32 @main() {
    entry:
      %tid = alloca i64
      %slot = alloca i8*
      store i8* bitcast (i8* (i8*)* @worker to i8*), i8** %slot
      %ctx = bitcast i8** %slot to i8*
      %rc = call i32 @pthread_create(i64* %tid, i8* null, i8* (i8*)* @worker, i8* %ctx)
      %loaded = load i8*, i8** %slot
      ret i32 %rc
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

  mem_value_t loaded_values;
  PTG->getLoadValues(Load->getPointerOperand(), Load, loaded_values);
  PTG->refineResult(loaded_values);

  bool saw_worker_ptr = false;
  for (const auto &item : loaded_values) {
    if (item.val == findValueByName(*Main, "ctx"))
      continue;
    if (auto *ce = dyn_cast<ConstantExpr>(item.val)) {
      if (ce->getOpcode() == Instruction::BitCast &&
          ce->getOperand(0) == M->getFunction("worker")) {
        saw_worker_ptr = true;
      }
    }
  }

  EXPECT_FALSE(saw_worker_ptr);
}
TEST(LotusAA, TopologicalTraversalDropsCyclicBlocksLikeLegacyTraversal) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br label %loop
    loop:
      br i1 %cond, label %loop, label %exit
    exit:
      ret void
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
  EXPECT_EQ(PTG->topBBs.size(), 1u);
  EXPECT_EQ(PTG->topBBs.front(), &Main->getEntryBlock());
}
TEST(LotusAA, NoValueStoresUseLegacySentinelType) {
  const char *IR = R"(
    declare void @ext(i8**)

    define void @main(i8** %slot) {
    entry:
      call void @ext(i8** %slot)
      ret void
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

  auto *Slot = dyn_cast<Argument>(findValueByName(*Main, "slot"));
  ASSERT_NE(Slot, nullptr);
  PTResult *slot_pts = PTG->findPTResult(Slot, false);
  ASSERT_NE(slot_pts, nullptr);

  PTResultIterator iter(slot_pts, PTG);
  ASSERT_EQ(iter.size(), 1);
  MemObject *obj = iter.begin()->first->getObj();
  auto updated_it = obj->getUpdatedOffset().find(0);
  ASSERT_NE(updated_it, obj->getUpdatedOffset().end());
  EXPECT_TRUE(updated_it->second->isIntegerTy(8));
}
TEST(LotusAA, GlobalSideEffectOutputReplaysIntoCaller) {
  const char *IR = R"(
    @slot = global i8* null

    define void @set_global(i8* %p) {
    entry:
      store i8* %p, i8** @slot
      ret void
    }

    define i8* @main(i8* %a) {
    entry:
      call void @set_global(i8* %a)
      %loaded = load i8*, i8** @slot
      ret i8* %loaded
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);
  Value *ArgA = findValueByName(*Main, "a");
  ASSERT_NE(ArgA, nullptr);

  PTResult *LoadPts = PTG->findPTResult(Load, false);
  PTResult *ArgPts = PTG->findPTResult(ArgA, false);
  ASSERT_NE(LoadPts, nullptr);
  ASSERT_NE(ArgPts, nullptr);

  PTResultIterator load_iter(LoadPts, PTG);
  PTResultIterator arg_iter(ArgPts, PTG);
  ASSERT_EQ(load_iter.size(), 1);
  ASSERT_EQ(arg_iter.size(), 1);
  EXPECT_EQ(load_iter.begin()->first, arg_iter.begin()->first);
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
TEST(LotusAA, PointerCompatibleIndirectTargetsAreAccepted) {
  const char *IR = R"(
    define i8* @good(i8* %x) {
    entry:
      ret i8* %x
    }

    define i32* @main(i32* %arg, i32* (i32*)* %fp) {
    entry:
      %res = call i32* %fp(i32* %arg)
      ret i32* %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Good = M->getFunction("good");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Good, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);
  CallBase *Call = Calls.front();

  CallTargetSet targets;
  targets[Good] = nullptr;
  Pass->getFunctionPointerResults().setTargets(Main, Call, targets);

  Pass->computePTA(Good);
  Pass->computePTA(Main);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  ASSERT_TRUE(PTG->func_arg.count(Call));
  EXPECT_TRUE(PTG->func_arg[Call].count(Good));
}
TEST(LotusAA, CallTargetConditionsAreExclusiveForSameIndirectCall) {
  const char *IR = R"(
    define i8* @foo(i8* %x) {
    entry:
      ret i8* %x
    }

    define i8* @bar(i8* %x) {
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
  Function *Foo = M->getFunction("foo");
  Function *Bar = M->getFunction("bar");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Foo, nullptr);
  ASSERT_NE(Bar, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);
  CallBase *Call = Calls.front();

  path_cond_t foo_cond = PTG->getCallTargetCond(Call->getCalledOperand(), Foo);
  path_cond_t bar_cond = PTG->getCallTargetCond(Call->getCalledOperand(), Bar);
  path_cond_t combined = PTG->findOrCreateAndRegion(foo_cond, bar_cond);

  ASSERT_NE(combined, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(foo_cond));
  EXPECT_TRUE(PTG->isSatisfiable(bar_cond));
  EXPECT_FALSE(PTG->isSatisfiable(combined));
}
TEST(LotusAA, PseudoOutputFunctionPointerFlowsToCallerIndirectCall) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define void @setfp(i1 %cond, i32 (i32)** %slot) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32 (i32)* @foo, i32 (i32)** %slot
      ret void
    else:
      store i32 (i32)* @bar, i32 (i32)** %slot
      ret void
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %slot = alloca i32 (i32)*
      call void @setfp(i1 %cond, i32 (i32)** %slot)
      %fp = load i32 (i32)*, i32 (i32)** %slot
      %res = call i32 %fp(i32 %x)
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
TEST(LotusAA, FullyInlineInterfaceCapsDepthAtConfiguredMaximum) {
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
  EXPECT_TRUE(containsImportedAtom(cond_a));
  EXPECT_TRUE(containsImportedAtom(cond_b));
  Value *Inner = findValueByName(*Main, "inner");
  ASSERT_NE(Inner, nullptr);
  EXPECT_FALSE(containsValueAtom(cond_a, Inner, true));
  EXPECT_FALSE(containsValueAtom(cond_b, Inner, false));
}
TEST(LotusAA, CompositeImportedSummaryGuardStaysOpaque) {
  const char *IR = R"(
    define i8* @choose(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    right:
      ret i8* %c
    }

    define i8* @main(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      %call = call i8* @choose(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c)
      ret i8* %call
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
  for (const auto &ret_pair : ChoosePTG->outputs.front()->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == findValueByName(*Choose, "a"))
        callee_cond_a = item.cond;
    }
  }

  ASSERT_NE(callee_cond_a, nullptr);
  ASSERT_EQ(callee_cond_a->getKind(), PathCond::Kind::And);

  path_cond_t imported = MainPTG->importPathCond(callee_cond_a, Call, Choose);
  ASSERT_NE(imported, nullptr);
  EXPECT_EQ(imported->getKind(), PathCond::Kind::ImportedAtom);
  EXPECT_EQ(imported->getImportedSource(), callee_cond_a);
  EXPECT_TRUE(containsImportedAtom(imported));

  Value *Outer = findValueByName(*Choose, "outer");
  Value *Inner = findValueByName(*Choose, "inner");
  ASSERT_NE(Outer, nullptr);
  ASSERT_NE(Inner, nullptr);
  EXPECT_FALSE(containsValueAtom(imported, Outer, true));
  EXPECT_FALSE(containsValueAtom(imported, Inner, true));
}
TEST(LotusAA, CompositeImportedFunctionSummaryStillResolvesIndirectTargets) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @baz(i32 %x) {
    entry:
      %dec = sub i32 %x, 1
      ret i32 %dec
    }

    define i32 (i32)* @choose(i1 %outer, i1 %inner) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %else
    then:
      ret i32 (i32)* @foo
    else:
      ret i32 (i32)* @bar
    right:
      ret i32 (i32)* @baz
    }

    define i32 @main(i1 %outer, i1 %inner, i32 %x) {
    entry:
      %fp = call i32 (i32)* @choose(i1 %outer, i1 %inner)
      %res = call i32 %fp(i32 %x)
      ret i32 %res
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
  computeAllFunctionCgs(*M, *Pass);

  auto *ChooseCall = findCallByCallee(*Main, "choose");
  ASSERT_NE(ChooseCall, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;

  auto FooIt = Targets.find(M->getFunction("foo"));
  auto BarIt = Targets.find(M->getFunction("bar"));
  auto BazIt = Targets.find(M->getFunction("baz"));
  ASSERT_NE(FooIt, Targets.end());
  ASSERT_NE(BarIt, Targets.end());
  ASSERT_NE(BazIt, Targets.end());

  EXPECT_TRUE(containsImportedAtom(FooIt->second));
  EXPECT_TRUE(containsImportedAtom(BarIt->second));
  EXPECT_TRUE(containsImportedAtom(BazIt->second));
}
TEST(LotusAA, CallerCgInliningResolvesTransitiveIndirectSetterTargets) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define void @setfoo(i32 (i32)** %slot) {
    entry:
      store i32 (i32)* @foo, i32 (i32)** %slot
      ret void
    }

    define void @setbar(i32 (i32)** %slot) {
    entry:
      store i32 (i32)* @bar, i32 (i32)** %slot
      ret void
    }

    define void @apply(i1 %cond,
                       void (i32 (i32)**)* %setter_true,
                       void (i32 (i32)**)* %setter_false,
                       i32 (i32)** %slot) {
    entry:
      %setter = select i1 %cond,
                       void (i32 (i32)**)* %setter_true,
                       void (i32 (i32)**)* %setter_false
      call void %setter(i32 (i32)** %slot)
      ret void
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %slot = alloca i32 (i32)*
      call void @apply(i1 %cond,
                       void (i32 (i32)**)* @setfoo,
                       void (i32 (i32)**)* @setbar,
                       i32 (i32)** %slot)
      %fp = load i32 (i32)*, i32 (i32)** %slot
      %res = call i32 %fp(i32 %x)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Apply = M->getFunction("apply");
  Function *Main = M->getFunction("main");
  ASSERT_NE(Apply, nullptr);
  ASSERT_NE(Main, nullptr);
  auto ApplyCalls = getIndirectCalls(*Apply);
  ASSERT_EQ(ApplyCalls.size(), 1u);

  auto *ApplyPTG = Pass->getPtGraph(Apply);
  ASSERT_NE(ApplyPTG, nullptr);
  ApplyPTG->computeCG();
  auto ApplyIt = ApplyPTG->cg_resolve_result.find(ApplyCalls.front());
  ASSERT_NE(ApplyIt, ApplyPTG->cg_resolve_result.end());
  EXPECT_TRUE(ApplyIt->second.empty());

  auto MainCalls = getIndirectCalls(*Main);
  ASSERT_EQ(MainCalls.size(), 1u);

  auto *MainPTG = Pass->getPtGraph(Main);
  ASSERT_NE(MainPTG, nullptr);
  MainPTG->computeCG();

  ApplyIt = ApplyPTG->cg_resolve_result.find(ApplyCalls.front());
  ASSERT_NE(ApplyIt, ApplyPTG->cg_resolve_result.end());
  EXPECT_EQ(ApplyIt->second.size(), 2u);
  EXPECT_TRUE(ApplyIt->second.count(M->getFunction("setfoo")));
  EXPECT_TRUE(ApplyIt->second.count(M->getFunction("setbar")));

  auto MainIt = MainPTG->cg_resolve_result.find(MainCalls.front());
  ASSERT_NE(MainIt, MainPTG->cg_resolve_result.end());
  EXPECT_TRUE(MainIt->second.empty());
}
TEST(LotusAA, UnmodeledSelectGuardKeepsIndirectCallOpaque) {
  const char *IR = R"(
    declare void @foo()
    declare void @bar()

    define void @main() {
    entry:
      %fp = select i1 undef, void ()* @foo, void ()* @bar
      call void %fp()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  PTG->computeCG();

  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  EXPECT_TRUE(It->second.empty());
}
TEST(LotusAA, PthreadCreateThreadArgCgInliningResolvesWorkerIndirectCall) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;

  const char *IR = R"(
    declare i32 @pthread_create(i64*, i8*, i8* (i8*)*, i8*)

    define void @foo() {
    entry:
      ret void
    }

    define i8* @worker(i8* %ctx) {
    entry:
      %slot = bitcast i8* %ctx to void ()**
      %fp = load void ()*, void ()** %slot
      call void %fp()
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i64
      %slot = alloca void ()*
      store void ()* @foo, void ()** %slot
      %ctx = bitcast void ()** %slot to i8*
      %rc = call i32 @pthread_create(i64* %tid, i8* null, i8* (i8*)* @worker, i8* %ctx)
      ret i32 %rc
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  computeAllFunctionCgs(*M, *Pass);

  auto *Worker = M->getFunction("worker");
  ASSERT_NE(Worker, nullptr);
  auto Calls = getIndirectCalls(*Worker);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Worker);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  EXPECT_TRUE(It->second.count(M->getFunction("foo")));
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
