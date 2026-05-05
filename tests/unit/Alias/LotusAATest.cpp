#include "LotusAATestSupport.h"

TEST(LotusAATest, AssignsDenseExplicitPseudoInputIndices) {
  const char *IR = R"(
    define void @callee(i32** %p, i32** %q) {
    entry:
      %a = load i32*, i32** %p
      %b = load i32*, i32** %q
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  std::unique_ptr<LotusAA> Pass(runLotusAA(*M));
  Function *F = M->getFunction("callee");
  ASSERT_NE(F, nullptr);

  IntraLotusAA *PTG = Pass->getPtGraph(F);
  ASSERT_NE(PTG, nullptr);
  ASSERT_EQ(PTG->getInputs().size(), 2u);

  unsigned expected_index = 0;
  for (const auto &input_item : PTG->getInputs()) {
    EXPECT_TRUE(PTG->isPseudoInput(input_item.first));
    EXPECT_EQ(PTG->getPseudoInputIndex(input_item.first),
              static_cast<int>(expected_index));
    ++expected_index;
  }
}
TEST(AliasAnalysisWrapperTest, RejectsUnimplementedAserPTAConfig) {
  const char *IR = R"(
    define i32 @test(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  lotus::AliasAnalysisWrapper Wrapper(*M, lotus::AAConfig::AserPTA_NoCtx());
  EXPECT_FALSE(Wrapper.isInitialized());
}
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
TEST(LotusAA, ReturnedFunctionPointerReachesCallerIndirectCall) {
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

    define i32 (i32)* @choose(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i32 (i32)* @foo
    else:
      ret i32 (i32)* @bar
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %fp = call i32 (i32)* @choose(i1 %cond)
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
TEST(LotusAA, ReturnedFunctionPointerImportsCallerLocalGuards) {
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

    define i32 (i32)* @choose(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i32 (i32)* @foo
    else:
      ret i32 (i32)* @bar
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %fp = call i32 (i32)* @choose(i1 %cond)
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

  Value *ChooseCond = findValueByName(*Choose, "cond");
  ASSERT_NE(ChooseCond, nullptr);

  auto FooIt = Targets.find(M->getFunction("foo"));
  auto BarIt = Targets.find(M->getFunction("bar"));
  ASSERT_NE(FooIt, Targets.end());
  ASSERT_NE(BarIt, Targets.end());

  EXPECT_TRUE(containsImportedAtom(FooIt->second));
  EXPECT_TRUE(containsImportedAtom(BarIt->second));
  EXPECT_FALSE(containsValueAtom(FooIt->second, ChooseCond, true));
  EXPECT_FALSE(containsValueAtom(BarIt->second, ChooseCond, false));
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
TEST(LotusAA, VariableIndexGepUsesUnknownOffsetBucket) {
  const char *IR = R"(
    define void @main(i64 %idx) {
    entry:
      %arr = alloca [2 x i8*]
      %f0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 0
      %f1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 1
      %dyn = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 %idx
      ret void
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

  Value *F0 = findValueByName(*Main, "f0");
  Value *F1 = findValueByName(*Main, "f1");
  Value *Dyn = findValueByName(*Main, "dyn");
  ASSERT_NE(F0, nullptr);
  ASSERT_NE(F1, nullptr);
  ASSERT_NE(Dyn, nullptr);

  PTResult *F0Pts = PTG->findPTResult(F0, false);
  PTResult *F1Pts = PTG->findPTResult(F1, false);
  PTResult *DynPts = PTG->findPTResult(Dyn, false);
  ASSERT_NE(F0Pts, nullptr);
  ASSERT_NE(F1Pts, nullptr);
  ASSERT_NE(DynPts, nullptr);

  PTResultIterator f0_iter(F0Pts, PTG);
  PTResultIterator f1_iter(F1Pts, PTG);
  PTResultIterator dyn_iter(DynPts, PTG);
  ASSERT_EQ(f0_iter.size(), 1);
  ASSERT_EQ(f1_iter.size(), 1);
  ASSERT_EQ(dyn_iter.size(), 1);

  EXPECT_EQ(f0_iter.begin()->first->getOffset(), 0);
  EXPECT_NE(f1_iter.begin()->first->getOffset(), 0);
  EXPECT_FALSE(PTGraph::isUnknownOffset(f1_iter.begin()->first->getOffset()));
  EXPECT_EQ(dyn_iter.begin()->first->getOffset(), 0);
  EXPECT_EQ(dyn_iter.begin()->first, f0_iter.begin()->first);
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
TEST(LotusAA, BranchNegationKeepsSiblingEdgeIdentity) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret void
    else:
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

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Then = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *Else = Entry->getTerminator()->getSuccessor(1);
  path_cond_t then_cond = PTG->getCFGEdgeCond(Entry, Then);
  path_cond_t else_cond = PTG->getCFGEdgeCond(Entry, Else);
  path_cond_t negated = PTG->findOrCreateNotRegion(then_cond);

  ASSERT_NE(negated, nullptr);
  EXPECT_EQ(negated->getKind(), PathCond::Kind::BranchAtom);
  EXPECT_EQ(negated, else_cond);
  EXPECT_TRUE(containsBranchAtom(negated, Entry, Else));
}
TEST(LotusAA, SameBooleanGuardAcrossControllersUsesLegacyContradictions) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      br i1 %cond, label %left_then, label %left_else
    left_then:
      ret void
    left_else:
      ret void
    right:
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

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Left = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *LeftElse = Left->getTerminator()->getSuccessor(1);

  path_cond_t entry_true = PTG->getCFGEdgeCond(Entry, Left);
  path_cond_t left_false = PTG->getCFGEdgeCond(Left, LeftElse);
  path_cond_t combined = PTG->findOrCreateAndRegion(entry_true, left_false);

  ASSERT_NE(combined, nullptr);
  EXPECT_FALSE(PTG->isSatisfiable(combined));
}
TEST(LotusAA, SelectAndPhiRejectImpossibleBooleanMixes) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %sel = select i1 %cond, i8* %a, i8* %b
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %sel, %then ], [ %c, %else ]
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
  Value *A = findValueByName(*Main, "a");
  Value *B = findValueByName(*Main, "b");
  Value *C = findValueByName(*Main, "c");
  ASSERT_NE(Phi, nullptr);
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(C, nullptr);

  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  path_cond_t cond_c = nullptr;

  PTResultIterator iter(PhiPts, PTG);
  for (auto &pt_item : iter) {
    Value *alloc_site = pt_item.first->getObj()->getAllocSite();
    if (alloc_site == A)
      cond_a = pt_item.second;
    else if (alloc_site == B)
      cond_b = pt_item.second;
    else if (alloc_site == C)
      cond_c = pt_item.second;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  ASSERT_NE(cond_c, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(cond_a));
  EXPECT_FALSE(PTG->isSatisfiable(cond_b));
  EXPECT_TRUE(PTG->isSatisfiable(cond_c));
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
  EXPECT_TRUE(PTG->isSatisfiable(cond));
  EXPECT_TRUE(PTG->isAlwaysSatisfied(cond));
  EXPECT_EQ(cond, PTG->getEmptyCond());
}
TEST(LotusAA, ComplementaryBranchOrMatchesLegacySimpleSolver) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret void
    else:
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

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Then = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *Else = Entry->getTerminator()->getSuccessor(1);

  path_cond_t then_cond = PTG->getCFGEdgeCond(Entry, Then);
  path_cond_t else_cond = PTG->getCFGEdgeCond(Entry, Else);
  path_cond_t combined = PTG->findOrCreateOrRegion(then_cond, else_cond);

  ASSERT_NE(combined, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(combined));
  EXPECT_TRUE(PTG->isAlwaysSatisfied(combined));
  EXPECT_EQ(combined, PTG->getEmptyCond());
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

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Then = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *Else = Entry->getTerminator()->getSuccessor(1);
  EXPECT_FALSE(PTG->isSatisfiable(
      PTG->findOrCreateAndRegion(cond_a, PTG->getCFGEdgeCond(Entry, Else))));
  EXPECT_FALSE(PTG->isSatisfiable(
      PTG->findOrCreateAndRegion(cond_b, PTG->getCFGEdgeCond(Entry, Then))));
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

  ObjectLocator *field_loc =
      iter.begin()->first->getObj()->findLocator(0, true);
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
TEST(LotusAA, SymbolicSequentialGepCollapsesToLegacyBaseField) {
  const char *IR = R"(
    define i8* @main(i64 %idx, i8* %a, i8* %b) {
    entry:
      %arr = alloca [4 x i8*]
      %slot0 = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 0
      store i8* %a, i8** %slot0
      %sloti = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 %idx
      store i8* %b, i8** %sloti
      %loaded = load i8*, i8** %slot0
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

  auto *SlotI = findValueByName(*Main, "sloti");
  ASSERT_NE(SlotI, nullptr);
  PTResult *SlotPts = PTG->findPTResult(SlotI, false);
  ASSERT_NE(SlotPts, nullptr);

  PTResultIterator slot_iter(SlotPts, PTG);
  ASSERT_EQ(slot_iter.size(), 1u);
  EXPECT_EQ(slot_iter.begin()->first->getOffset(), 0);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);

  mem_value_t loaded_values;
  PTG->getLoadValues(Load->getPointerOperand(), Load, loaded_values);
  PTG->refineResult(loaded_values);

  bool saw_a = false;
  bool saw_b = false;
  for (const auto &item : loaded_values) {
    if (item.val == findValueByName(*Main, "a"))
      saw_a = true;
    if (item.val == findValueByName(*Main, "b"))
      saw_b = true;
  }

  EXPECT_FALSE(saw_a);
  EXPECT_TRUE(saw_b);
}
TEST(LotusAA, DisabledLibraryHeuristicPreservesPointerArgumentValues) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_disable_library_heuristic = true;

  const char *IR = R"(
    declare void @ext(i8**)

    define i8* @main(i8* %b) {
    entry:
      %slot = alloca i8*
      store i8* %b, i8** %slot
      call void @ext(i8** %slot)
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

  mem_value_t loaded_values;
  PTG->getLoadValues(Load->getPointerOperand(), Load, loaded_values);
  PTG->refineResult(loaded_values);

  bool saw_b = false;
  for (const auto &item : loaded_values) {
    if (item.val == findValueByName(*Main, "b"))
      saw_b = true;
  }
  EXPECT_TRUE(saw_b);
}
