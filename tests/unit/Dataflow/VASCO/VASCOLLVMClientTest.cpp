#include "Dataflow/VASCO/VASCO.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace {

bool containsLocation(const vasco::llvmir::MemoryLocationSet &Locations,
                      const vasco::llvmir::MemoryLocation &Needle) {
  for (const auto &Location : Locations) {
    if (Location.Object.value() == Needle.Object.value() &&
        Location.Object.kind() == Needle.Object.kind() &&
        Location.Offset == Needle.Offset &&
        Location.IsSummary == Needle.IsSummary) {
      return true;
    }
  }
  return false;
}

bool containsSummaryLocation(const vasco::llvmir::MemoryLocationSet &Locations) {
  for (const auto &Location : Locations) {
    if (Location.IsSummary || Location.Object.isSummary()) {
      return true;
    }
  }
  return false;
}

class VASCOLLVMClientTest : public ::testing::Test {
protected:
  llvm::LLVMContext Context;

  std::unique_ptr<llvm::Module> parse(const char *IR) {
    return lotus::unittest::parseModuleChecked(Context, IR, "VASCOTest");
  }
};

TEST_F(VASCOLLVMClientTest, SignAnalysisTracksRecursiveExample) {
  auto Module = parse(R"(
    define i32 @five() {
    entry:
      ret i32 5
    }

    define i32 @f(i32 %a, i32 %b) {
    entry:
      %cmp = icmp slt i32 %a, %b
      br i1 %cmp, label %mulb, label %callb
    mulb:
      %c.mul = mul i32 %a, %b
      br label %ret
    callb:
      %c.call = call i32 @g(i32 10)
      br label %ret
    ret:
      %c = phi i32 [ %c.mul, %mulb ], [ %c.call, %callb ]
      ret i32 %c
    }

    define i32 @g(i32 %u) {
    entry:
      %neg = sub i32 0, %u
      %v = call i32 @f(i32 %neg, i32 %u)
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = call i32 @five()
      %q = call i32 @f(i32 %p, i32 -3)
      %negq = sub i32 0, %q
      %r = call i32 @g(i32 %negq)
      ret i32 %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::SignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *F = Module->getFunction("f");
  auto *G = Module->getFunction("g");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(G, nullptr);

  const auto &FContexts = Analysis.getContexts(F);
  const auto &GContexts = Analysis.getContexts(G);
  ASSERT_EQ(FContexts.size(), 2U);
  ASSERT_EQ(GContexts.size(), 1U);

  auto *ArgIt = F->arg_begin();
  auto *AArg = &*ArgIt++;
  auto *BArg = &*ArgIt++;
  EXPECT_EQ(
      FContexts[0]->getExitValue().at(vasco::llvmir::ValueKey::returnValue()),
      vasco::llvmir::Sign::Negative);
  EXPECT_EQ(
      FContexts[1]->getExitValue().at(vasco::llvmir::ValueKey::returnValue()),
      vasco::llvmir::Sign::Negative);

  bool SawPositiveNegative = false;
  bool SawNegativePositive = false;
  for (const auto &Context : FContexts) {
    const auto &Entry = Context->getEntryValue();
    auto ASign = Entry.at(vasco::llvmir::ValueKey::forValue(AArg));
    auto BSign = Entry.at(vasco::llvmir::ValueKey::forValue(BArg));
    SawPositiveNegative |=
        ASign == vasco::llvmir::Sign::Positive &&
        BSign == vasco::llvmir::Sign::Negative;
    SawNegativePositive |=
        ASign == vasco::llvmir::Sign::Negative &&
        BSign == vasco::llvmir::Sign::Positive;
  }
  EXPECT_TRUE(SawPositiveNegative);
  EXPECT_TRUE(SawNegativePositive);

  auto *Main = Module->getFunction("main");
  auto *RCall = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(RCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(
      Solution.getValueAfter(RCall).at(vasco::llvmir::ValueKey::forValue(RCall)),
      vasco::llvmir::Sign::Negative);
}

TEST_F(VASCOLLVMClientTest, CopyConstantAnalysisTracksInterproceduralReturns) {
  auto Module = parse(R"(
    define i32 @id(i32 %a) {
    entry:
      ret i32 %a
    }

    define i32 @foo(i32 %a, i32 %b, i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %z = phi i32 [ %a, %then ], [ %b, %else ]
      ret i32 %z
    }

    define i32 @main() {
    entry:
      %x = call i32 @id(i32 8)
      %y = call i32 @foo(i32 8, i32 8, i1 true)
      ret i32 %y
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::CopyConstantAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Foo = Module->getFunction("foo");
  ASSERT_NE(Foo, nullptr);
  const auto &FooContexts = Analysis.getContexts(Foo);
  ASSERT_EQ(FooContexts.size(), 1U);

  const llvm::Constant *RetConstant =
      FooContexts.front()->getExitValue().at(vasco::llvmir::ValueKey::returnValue());
  auto *RetInt = llvm::dyn_cast<llvm::ConstantInt>(RetConstant);
  ASSERT_NE(RetInt, nullptr);
  EXPECT_EQ(RetInt->getSExtValue(), 8);

  auto *Main = Module->getFunction("main");
  auto *YCall = lotus::unittest::findInstructionByName(Main, "y");
  ASSERT_NE(YCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const llvm::Constant *YConstant =
      Solution.getValueAfter(YCall).at(vasco::llvmir::ValueKey::forValue(YCall));
  auto *YInt = llvm::dyn_cast<llvm::ConstantInt>(YConstant);
  ASSERT_NE(YInt, nullptr);
  EXPECT_EQ(YInt->getSExtValue(), 8);
}

TEST_F(VASCOLLVMClientTest, DefaultLLVMProgramRepresentationTreatsIndirectAsUnknown) {
  auto Module = parse(R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main(i32 (i32)* %fp) {
    entry:
      %r = call i32 %fp(i32 1)
      ret i32 %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  auto *Main = Module->getFunction("main");
  auto *Call = llvm::dyn_cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(Main, "r"));
  ASSERT_NE(Call, nullptr);

  auto Targets = Program.resolveTargets(Main, Call);
  EXPECT_FALSE(Targets.has_value());
}

TEST_F(VASCOLLVMClientTest, DefaultLLVMProgramRepresentationResolvesAliasedDirectCall) {
  auto Module = parse(R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    @callee.alias = alias i32 (i32), i32 (i32)* @callee

    define i32 @main() {
    entry:
      %r = call i32 @callee.alias(i32 7)
      ret i32 %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  auto *Main = Module->getFunction("main");
  auto *Call = llvm::dyn_cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(Main, "r"));
  ASSERT_NE(Call, nullptr);

  auto Targets = Program.resolveTargets(Main, Call);
  ASSERT_TRUE(Targets.has_value());
  ASSERT_EQ(Targets->size(), 1U);
  EXPECT_EQ(Targets->front(), Module->getFunction("callee"));
}

TEST_F(VASCOLLVMClientTest, UnknownCallsConservativelyKillCopyConstantResult) {
  auto Module = parse(R"(
    define i32 @main(i32 (i32)* %fp) {
    entry:
      %x = call i32 %fp(i32 1)
      ret i32 %x
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  vasco::llvmir::CopyConstantAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *XCall = lotus::unittest::findInstructionByName(Main, "x");
  ASSERT_NE(XCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(Solution.getValueAfter(XCall).at(vasco::llvmir::ValueKey::forValue(XCall)),
            nullptr);
}

TEST_F(VASCOLLVMClientTest, UnknownCallsConservativelyBottomSignResult) {
  auto Module = parse(R"(
    define i32 @main(i32 (i32)* %fp) {
    entry:
      %x = call i32 %fp(i32 1)
      ret i32 %x
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  vasco::llvmir::SignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *XCall = lotus::unittest::findInstructionByName(Main, "x");
  ASSERT_NE(XCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(Solution.getValueAfter(XCall).at(vasco::llvmir::ValueKey::forValue(XCall)),
            vasco::llvmir::Sign::Bottom);
}

TEST_F(VASCOLLVMClientTest, SignAnalysisHandlesUnreachableExitAsTail) {
  auto Module = parse(R"(
    define i32 @abort_like() {
    entry:
      unreachable
    }

    define i32 @main() {
    entry:
      %r = call i32 @abort_like()
      ret i32 0
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::SignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *AbortLike = Module->getFunction("abort_like");
  ASSERT_NE(AbortLike, nullptr);

  const auto &Contexts = Analysis.getContexts(AbortLike);
  ASSERT_EQ(Contexts.size(), 1U);
  EXPECT_TRUE(Contexts.front()->isAnalysed());
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisPropagatesReturnedAllocation) {
  auto Module = parse(R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* %p
    }

    define i8* @main() {
    entry:
      %slot = alloca i8*
      %obj = alloca i8
      store i8* %obj, i8** %slot
      %loaded = load i8*, i8** %slot
      %ret = call i8* @id(i8* %loaded)
      ret i8* %ret
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Obj = lotus::unittest::findInstructionByName(Main, "obj");
  auto *RetCall = lotus::unittest::findInstructionByName(Main, "ret");
  ASSERT_NE(Obj, nullptr);
  ASSERT_NE(RetCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &RetTargets = Solution.getValueAfter(RetCall).pointsTo(
      vasco::llvmir::PointsToValue::forValue(RetCall));
  EXPECT_TRUE(containsLocation(
      RetTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(Obj))));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisResolvesIndirectCallTargets) {
  auto Module = parse(R"(
    define i8* @ret_a(i8* %a, i8* %b) {
    entry:
      ret i8* %a
    }

    define i8* @ret_b(i8* %a, i8* %b) {
    entry:
      ret i8* %b
    }

    define i8* @main(i1 %cond) {
    entry:
      %a = alloca i8
      %b = alloca i8
      %fp = select i1 %cond, i8* (i8*, i8*)* @ret_a, i8* (i8*, i8*)* @ret_b
      %r = call i8* %fp(i8* %a, i8* %b)
      ret i8* %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *RetA = Module->getFunction("ret_a");
  auto *RetB = Module->getFunction("ret_b");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *Call = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(RetA, nullptr);
  ASSERT_NE(RetB, nullptr);
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(Call, nullptr);

  const auto &Transitions = Analysis.getContextTransitionTable().getTransitions();
  bool SawRetA = false;
  bool SawRetB = false;
  for (const auto &Entry : Transitions) {
    if (Entry.first.getCallNode() != Call) {
      continue;
    }
    SawRetA |= Entry.second.count(RetA) != 0;
    SawRetB |= Entry.second.count(RetB) != 0;
  }
  EXPECT_TRUE(SawRetA);
  EXPECT_TRUE(SawRetB);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &Targets = Solution.getValueAfter(Call).pointsTo(
      vasco::llvmir::PointsToValue::forValue(Call));
  EXPECT_TRUE(containsLocation(
      Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_TRUE(containsLocation(
      Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisMarksUnknownIndirectCallDefault) {
  auto Module = parse(R"(
    define i8* @main(i8* (i8*)* %fp, i8* %arg) {
    entry:
      %r = call i8* %fp(i8* %arg)
      ret i8* %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Call = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(Call, nullptr);

  const auto &DefaultSites =
      Analysis.getContextTransitionTable().getDefaultCallSites();
  bool SawDefaultCall = false;
  for (const auto &Site : DefaultSites) {
    SawDefaultCall |= Site.getCallNode() == Call;
  }
  EXPECT_TRUE(SawDefaultCall);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &Targets = Solution.getValueAfter(Call).pointsTo(
      vasco::llvmir::PointsToValue::forValue(Call));
  EXPECT_TRUE(containsLocation(
      Targets, vasco::llvmir::MemoryLocation::summary(vasco::llvmir::PointsToObject::summary())));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisIsFieldSensitiveForStructGEPs) {
  auto Module = parse(R"(
    %pair = type { i8*, i8* }

    define i8* @main() {
    entry:
      %s = alloca %pair
      %lhs = getelementptr inbounds %pair, %pair* %s, i32 0, i32 0
      %rhs = getelementptr inbounds %pair, %pair* %s, i32 0, i32 1
      %a = alloca i8
      %b = alloca i8
      store i8* %a, i8** %lhs
      store i8* %b, i8** %rhs
      %ra = load i8*, i8** %lhs
      %rb = load i8*, i8** %rhs
      ret i8* %ra
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *RA = lotus::unittest::findInstructionByName(Main, "ra");
  auto *RB = lotus::unittest::findInstructionByName(Main, "rb");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(RA, nullptr);
  ASSERT_NE(RB, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &RATargets =
      Solution.getValueAfter(RA).pointsTo(vasco::llvmir::PointsToValue::forValue(RA));
  const auto &RBTargets =
      Solution.getValueAfter(RB).pointsTo(vasco::llvmir::PointsToValue::forValue(RB));

  EXPECT_TRUE(containsLocation(
      RATargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_FALSE(containsLocation(
      RATargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
  EXPECT_TRUE(containsLocation(
      RBTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisKeepsCallerLocalsOutOfReturnState) {
  auto Module = parse(R"(
    define i8* @callee(i8* %p) {
    entry:
      ret i8* %p
    }

    define i8* @main() {
    entry:
      %local = alloca i8
      %ret = call i8* @callee(i8* %local)
      ret i8* %ret
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Local = lotus::unittest::findInstructionByName(Main, "local");
  auto *Ret = lotus::unittest::findInstructionByName(Main, "ret");
  auto *Callee = Module->getFunction("callee");
  ASSERT_NE(Local, nullptr);
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Callee, nullptr);

  const auto &Contexts = Analysis.getContexts(Callee);
  ASSERT_EQ(Contexts.size(), 1U);
  const auto &ExitRoots = Contexts.front()->getExitValue().getRoots();
  EXPECT_EQ(ExitRoots.count(vasco::llvmir::PointsToValue::forValue(Local)), 0U);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &RetTargets =
      Solution.getValueAfter(Ret).pointsTo(vasco::llvmir::PointsToValue::forValue(Ret));
  EXPECT_TRUE(containsLocation(
      RetTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(Local))));
}

TEST_F(VASCOLLVMClientTest, MemoryModelInfersTypedHeapLayoutFromBitcastUse) {
  auto Module = parse(R"(
    %pair = type { i8*, i8* }

    declare i8* @malloc(i64)

    define i8* @main() {
    entry:
      %raw = call i8* @malloc(i64 16)
      %pair.ptr = bitcast i8* %raw to %pair*
      %lhs = getelementptr inbounds %pair, %pair* %pair.ptr, i32 0, i32 0
      %rhs = getelementptr inbounds %pair, %pair* %pair.ptr, i32 0, i32 1
      %a = alloca i8
      %b = alloca i8
      store i8* %a, i8** %lhs
      store i8* %b, i8** %rhs
      %ra = load i8*, i8** %lhs
      %rb = load i8*, i8** %rhs
      ret i8* %ra
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Raw = lotus::unittest::findInstructionByName(Main, "raw");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *RA = lotus::unittest::findInstructionByName(Main, "ra");
  auto *RB = lotus::unittest::findInstructionByName(Main, "rb");
  ASSERT_NE(Raw, nullptr);
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(RA, nullptr);
  ASSERT_NE(RB, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &RawTargets =
      Solution.getValueAfter(Raw).pointsTo(vasco::llvmir::PointsToValue::forValue(Raw));
  const auto &RATargets =
      Solution.getValueAfter(RA).pointsTo(vasco::llvmir::PointsToValue::forValue(RA));
  const auto &RBTargets =
      Solution.getValueAfter(RB).pointsTo(vasco::llvmir::PointsToValue::forValue(RB));

  bool SawHeap = false;
  for (const auto &Location : RawTargets) {
    SawHeap |= Location.Object.kind() == vasco::llvmir::AllocationSiteKind::Heap &&
               Location.Object.value() == Raw &&
               Location.Object.Layout.FieldSensitive &&
               Location.Object.Layout.HasKnownSize &&
               Location.Object.Layout.Size == 16;
  }
  EXPECT_TRUE(SawHeap);
  EXPECT_TRUE(containsLocation(
      RATargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_TRUE(containsLocation(
      RBTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
}

TEST_F(VASCOLLVMClientTest, MemoryModelInfersAllocSizeAttributeHeapLayout) {
  auto Module = parse(R"(
    declare i8* @my_alloc(i64, i64) allocsize(0, 1)

    define i8* @main() {
    entry:
      %buf = call i8* @my_alloc(i64 4, i64 8)
      ret i8* %buf
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Buf = lotus::unittest::findInstructionByName(Main, "buf");
  ASSERT_NE(Buf, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &Targets =
      Solution.getValueAfter(Buf).pointsTo(vasco::llvmir::PointsToValue::forValue(Buf));

  bool SawHeap = false;
  for (const auto &Location : Targets) {
    SawHeap |= Location.Object.kind() == vasco::llvmir::AllocationSiteKind::Heap &&
               Location.Object.value() == Buf &&
               Location.Object.Layout.HasKnownSize &&
               Location.Object.Layout.Size == 32;
  }
  EXPECT_TRUE(SawHeap);
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisSummarizesMemcpyIntoDestination) {
  auto Module = parse(R"(
    %pair = type { i8*, i8* }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i8* @main() {
    entry:
      %src = alloca %pair
      %dst = alloca %pair
      %src0 = getelementptr inbounds %pair, %pair* %src, i32 0, i32 0
      %src1 = getelementptr inbounds %pair, %pair* %src, i32 0, i32 1
      %dst0 = getelementptr inbounds %pair, %pair* %dst, i32 0, i32 0
      %dst1 = getelementptr inbounds %pair, %pair* %dst, i32 0, i32 1
      %a = alloca i8
      %b = alloca i8
      store i8* %a, i8** %src0
      store i8* %b, i8** %src1
      %src.i8 = bitcast %pair* %src to i8*
      %dst.i8 = bitcast %pair* %dst to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst.i8, i8* %src.i8, i64 16, i1 false)
      %ra = load i8*, i8** %dst0
      %rb = load i8*, i8** %dst1
      ret i8* %ra
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *RA = lotus::unittest::findInstructionByName(Main, "ra");
  auto *RB = lotus::unittest::findInstructionByName(Main, "rb");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(RA, nullptr);
  ASSERT_NE(RB, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &RATargets =
      Solution.getValueAfter(RA).pointsTo(vasco::llvmir::PointsToValue::forValue(RA));
  const auto &RBTargets =
      Solution.getValueAfter(RB).pointsTo(vasco::llvmir::PointsToValue::forValue(RB));
  EXPECT_TRUE(containsLocation(
      RATargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_TRUE(containsLocation(
      RBTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisModelsReallocAsHeapUpdate) {
  auto Module = parse(R"(
    %pair = type { i8*, i8* }
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)

    define i8* @main() {
    entry:
      %raw = call i8* @malloc(i64 16)
      %pair.ptr = bitcast i8* %raw to %pair*
      %lhs = getelementptr inbounds %pair, %pair* %pair.ptr, i32 0, i32 0
      %a = alloca i8
      store i8* %a, i8** %lhs
      %grown = call i8* @realloc(i8* %raw, i64 32)
      %grown.pair = bitcast i8* %grown to %pair*
      %grown.lhs = getelementptr inbounds %pair, %pair* %grown.pair, i32 0, i32 0
      %r = load i8*, i8** %grown.lhs
      ret i8* %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *Grown = lotus::unittest::findInstructionByName(Main, "grown");
  auto *R = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(Grown, nullptr);
  ASSERT_NE(R, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &GrownTargets =
      Solution.getValueAfter(Grown).pointsTo(vasco::llvmir::PointsToValue::forValue(Grown));
  bool SawSizedHeap = false;
  for (const auto &Location : GrownTargets) {
    SawSizedHeap |= Location.Object.kind() == vasco::llvmir::AllocationSiteKind::Heap &&
                    Location.Object.value() == Grown &&
                    Location.Object.Layout.HasKnownSize &&
                    Location.Object.Layout.Size == 32;
  }
  EXPECT_TRUE(SawSizedHeap);

  const auto &RTargets =
      Solution.getValueAfter(R).pointsTo(vasco::llvmir::PointsToValue::forValue(R));
  EXPECT_TRUE(containsLocation(
      RTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisModelsAllocatorOutParameters) {
  auto Module = parse(R"(
    declare i32 @posix_memalign(i8**, i64, i64)

    define i8* @main() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %rc = call i32 @posix_memalign(i8** %slot, i64 16, i64 32)
      %buf = load i8*, i8** %slot
      ret i8* %buf
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Call = lotus::unittest::findInstructionByName(Main, "rc");
  auto *Buf = lotus::unittest::findInstructionByName(Main, "buf");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Buf, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &Targets =
      Solution.getValueAfter(Buf).pointsTo(vasco::llvmir::PointsToValue::forValue(Buf));

  bool SawHeap = false;
  for (const auto &Location : Targets) {
    SawHeap |= Location.Object.kind() == vasco::llvmir::AllocationSiteKind::Heap &&
               Location.Object.value() == Call;
  }
  EXPECT_TRUE(SawHeap);
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisUsesReturnedAttributeOnExternalCall) {
  auto Module = parse(R"(
    declare i8* @identity_ext(i8* returned)

    define i8* @main() {
    entry:
      %obj = alloca i8
      %ret = call i8* @identity_ext(i8* %obj)
      ret i8* %ret
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Obj = lotus::unittest::findInstructionByName(Main, "obj");
  auto *Ret = lotus::unittest::findInstructionByName(Main, "ret");
  ASSERT_NE(Obj, nullptr);
  ASSERT_NE(Ret, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &Targets =
      Solution.getValueAfter(Ret).pointsTo(vasco::llvmir::PointsToValue::forValue(Ret));
  EXPECT_TRUE(containsLocation(
      Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(Obj))));
  EXPECT_FALSE(containsSummaryLocation(Targets));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisPreservesReadonlyUnknownCallPrecision) {
  auto Module = parse(R"(
    %pair = type { i8*, i8* }
    declare void @inspect(%pair* nocapture readonly)

    define i8* @main() {
    entry:
      %s = alloca %pair
      %lhs = getelementptr inbounds %pair, %pair* %s, i32 0, i32 0
      %rhs = getelementptr inbounds %pair, %pair* %s, i32 0, i32 1
      %a = alloca i8
      %b = alloca i8
      store i8* %a, i8** %lhs
      store i8* %b, i8** %rhs
      call void @inspect(%pair* %s)
      %ra = load i8*, i8** %lhs
      %rb = load i8*, i8** %rhs
      ret i8* %ra
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *RA = lotus::unittest::findInstructionByName(Main, "ra");
  auto *RB = lotus::unittest::findInstructionByName(Main, "rb");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(RA, nullptr);
  ASSERT_NE(RB, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &RATargets =
      Solution.getValueAfter(RA).pointsTo(vasco::llvmir::PointsToValue::forValue(RA));
  const auto &RBTargets =
      Solution.getValueAfter(RB).pointsTo(vasco::llvmir::PointsToValue::forValue(RB));

  EXPECT_TRUE(containsLocation(
      RATargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_TRUE(containsLocation(
      RBTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
  EXPECT_FALSE(containsSummaryLocation(RATargets));
  EXPECT_FALSE(containsSummaryLocation(RBTargets));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisJoinsMemcpyFromMultipleSources) {
  auto Module = parse(R"(
    %box = type { i8* }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i8* @main(i1 %cond) {
    entry:
      %src1 = alloca %box
      %src2 = alloca %box
      %dst = alloca %box
      %src1.field = getelementptr inbounds %box, %box* %src1, i32 0, i32 0
      %src2.field = getelementptr inbounds %box, %box* %src2, i32 0, i32 0
      %dst.field = getelementptr inbounds %box, %box* %dst, i32 0, i32 0
      %a = alloca i8
      %b = alloca i8
      store i8* %a, i8** %src1.field
      store i8* %b, i8** %src2.field
      %chosen = select i1 %cond, %box* %src1, %box* %src2
      %chosen.i8 = bitcast %box* %chosen to i8*
      %dst.i8 = bitcast %box* %dst to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst.i8, i8* %chosen.i8, i64 8, i1 false)
      %r = load i8*, i8** %dst.field
      ret i8* %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *R = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(R, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &Targets =
      Solution.getValueAfter(R).pointsTo(vasco::llvmir::PointsToValue::forValue(R));
  EXPECT_TRUE(containsLocation(
      Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_TRUE(containsLocation(
      Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisKeepsAllReallocOutcomes) {
  auto Module = parse(R"(
    %box = type { i8* }
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)

    define i8* @main(i1 %cond) {
    entry:
      %raw1 = call i8* @malloc(i64 8)
      %raw2 = call i8* @malloc(i64 8)
      %box1 = bitcast i8* %raw1 to %box*
      %box2 = bitcast i8* %raw2 to %box*
      %f1 = getelementptr inbounds %box, %box* %box1, i32 0, i32 0
      %f2 = getelementptr inbounds %box, %box* %box2, i32 0, i32 0
      %a = alloca i8
      %b = alloca i8
      store i8* %a, i8** %f1
      store i8* %b, i8** %f2
      %old = select i1 %cond, i8* %raw1, i8* %raw2
      %grown = call i8* @realloc(i8* %old, i64 16)
      %grown.box = bitcast i8* %grown to %box*
      %grown.field = getelementptr inbounds %box, %box* %grown.box, i32 0, i32 0
      %r = load i8*, i8** %grown.field
      ret i8* %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *R = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(R, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &RTargets =
      Solution.getValueAfter(R).pointsTo(vasco::llvmir::PointsToValue::forValue(R));
  EXPECT_TRUE(containsLocation(
      RTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_TRUE(containsLocation(
      RTargets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
}

TEST_F(VASCOLLVMClientTest, PointsToAnalysisRespectsMemcpySourceAndDestinationOffsets) {
  auto Module = parse(R"(
    %pair = type { i8*, i8* }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i8* @main() {
    entry:
      %src = alloca %pair
      %dst = alloca %pair
      %src0 = getelementptr inbounds %pair, %pair* %src, i32 0, i32 0
      %src1 = getelementptr inbounds %pair, %pair* %src, i32 0, i32 1
      %dst0 = getelementptr inbounds %pair, %pair* %dst, i32 0, i32 0
      %dst1 = getelementptr inbounds %pair, %pair* %dst, i32 0, i32 1
      %a = alloca i8
      %b = alloca i8
      store i8* %a, i8** %src0
      store i8* %b, i8** %src1
      %src1.i8 = bitcast i8** %src1 to i8*
      %dst0.i8 = bitcast i8** %dst0 to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst0.i8, i8* %src1.i8, i64 8, i1 false)
      %r0 = load i8*, i8** %dst0
      %r1 = load i8*, i8** %dst1
      ret i8* %r0
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::PointsToAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *A = lotus::unittest::findInstructionByName(Main, "a");
  auto *B = lotus::unittest::findInstructionByName(Main, "b");
  auto *R0 = lotus::unittest::findInstructionByName(Main, "r0");
  auto *R1 = lotus::unittest::findInstructionByName(Main, "r1");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(R0, nullptr);
  ASSERT_NE(R1, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &R0Targets =
      Solution.getValueAfter(R0).pointsTo(vasco::llvmir::PointsToValue::forValue(R0));
  const auto &R1Targets =
      Solution.getValueAfter(R1).pointsTo(vasco::llvmir::PointsToValue::forValue(R1));

  EXPECT_TRUE(containsLocation(
      R0Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
  EXPECT_FALSE(containsLocation(
      R0Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(A))));
  EXPECT_FALSE(containsLocation(
      R1Targets,
      vasco::llvmir::MemoryLocation::exact(vasco::llvmir::PointsToObject::stack(B))));
}

} // namespace
