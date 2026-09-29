#include "IR/UseTraceSSA/SVFGBridge.h"
#include "IR/UseTraceSSA/DefectDetector.h"
#include "IR/UseTraceSSA/LLVMImporter.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace lotus::usetracessa;
namespace {
TEST(UseTraceSSANative, LabelsPreserveUnnamedValuesAndMetadata) {
  llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"IR(
    declare void @free(i8*)
    define i8 @first(i8* %0) {
      %2 = load i8, i8* %0, !tag !0
      call void @free(i8* %0)
      ret i8 %2
    }
    define i8 @second(i8* %0) {
      %2 = getelementptr i8, i8* %0, i64 1
      %3 = load i8, i8* %2, !tag !1
      switch i8 %3, label %done [ i8 0, label %zero ]
    zero:
      ret i8 0
    done:
      ret i8 %3
    }
    !0 = !{i32 17}
    !1 = !{i32 29}
  )IR", diagnostic, context);
  ASSERT_TRUE(module);
  std::string moduleText;
  llvm::raw_string_ostream moduleOut(moduleText);
  module->print(moduleOut, nullptr);
  moduleOut.flush();
  lotus::analysis::SVFG svfg;
  for (auto mode : {NativeHistoryMode::Full, NativeHistoryMode::UseAfterFree}) {
    auto imported = buildUseTraceSSAFromLotusSVFG(svfg, *module, mode);
    for (const auto &function : *module) {
      auto history = LLVMHistoryBuilder::build(function);
      for (const auto &block : function) for (const auto &instruction : block) {
        const auto &expected =
            history.graph().program().operations().at(history.siteID(instruction)).label;
        // Metadata slots now consistently refer to the containing module,
        // including when the next function has a different metadata attachment.
        EXPECT_NE(moduleText.find(expected), std::string::npos) << expected;
        if (mode != NativeHistoryMode::Full && !llvm::isa<llvm::LoadInst>(instruction)) continue;
        bool present = false;
        for (const auto &node : imported.graph.nodes())
          present |= node.label.find(expected) != std::string::npos;
        EXPECT_TRUE(present) << expected;
      }
    }
  }
}
TEST(UseTraceSSANative, SharedCallsAndUnknownFacts) {
  llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
  auto m = llvm::parseAssemblyString(R"IR(
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
  )IR",diagnostic,context);
  ASSERT_TRUE(m);
  lotus::analysis::SVFG svfg;
  for (const char *name : {"caller","release"})
    svfg.addObjectForValue(m->getFunction(name)->getArg(0),42);
  for (auto mode : {NativeHistoryMode::Full,NativeHistoryMode::UseAfterFree}) {
    auto r=buildUseTraceSSAFromLotusSVFG(svfg,*m,mode);
    EXPECT_TRUE(r.graph.verify());
    auto q=queries::useAfterFree(r.graph); q.memoryObject=42;
    auto fixed=QueryEngine(r.graph).run(q); EXPECT_TRUE(fixed.found());
    bool returned=false;
    for (auto id : fixed.edges) returned |= r.graph.edge(id).kind==FlowKind::Return;
    EXPECT_TRUE(returned);
    q.memoryObject.reset();
    auto batch=QueryEngine(r.graph).runObjects({q,ObjectUniverse({42,99,UnknownResource})});
    EXPECT_EQ(batch.status(42),fixed.status);
    for (auto object : {ObjectID(99),UnknownResource}) {
      q.memoryObject=object;
      EXPECT_EQ(batch.status(object),QueryEngine(r.graph).run(q).status);
    }
    EXPECT_EQ(DefectDetector(r.graph).scan(DefectKind::UseAfterFree).status,QueryStatus::Found);
  }
  lotus::analysis::SVFG unknown;
  auto r=buildUseTraceSSAFromLotusSVFG(unknown,*m,NativeHistoryMode::UseAfterFree);
  EXPECT_EQ(r.graph.resourceCandidates(),std::vector<ObjectID>({UnknownResource}));
  EXPECT_EQ(DefectDetector(r.graph).run(DefectKind::UseAfterFree).result.status,QueryStatus::Found);
  for (const auto &issue : r.graph.issues())
    EXPECT_EQ(issue.find("points-to set is unknown"),std::string::npos);
}
TEST(UseTraceSSANative, PropertySelectsEventsSitesAndObjectUniverse) {
  llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"IR(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare i8* @fopen(i8*, i8*)
    declare i32 @fclose(i8*)
    declare void @llvm.assume(i1)
    define void @mixed(i8* %unrelated, i8* %unknown) {
      %heap = call i8* @malloc(i64 8)
      %heap_leak = call i8* @malloc(i64 8)
      call void @free(i8* %heap)
      call void @free(i8* %heap)
      %used = load i8, i8* %heap
      %irrelevant = load i8, i8* %unrelated
      %possible_use = load i8, i8* %unknown
      %file = call i8* @fopen(i8* null, i8* null)
      %file_leak = call i8* @fopen(i8* null, i8* null)
      %closed = call i32 @fclose(i8* %file)
      call void @llvm.assume(i1 true)
      ret void
    }
  )IR", diagnostic, context);
  ASSERT_TRUE(module);
  auto &function = *module->getFunction("mixed");
  lotus::analysis::SVFG svfg;
  svfg.addObjectForValue(function.getArg(0), 99);
  for (const auto &block : function) for (const auto &instruction : block) {
    if (instruction.getName() == "heap") svfg.addObjectForValue(&instruction, 10);
    if (instruction.getName() == "heap_leak") svfg.addObjectForValue(&instruction, 11);
    if (instruction.getName() == "file") svfg.addObjectForValue(&instruction, 20);
    if (instruction.getName() == "file_leak") svfg.addObjectForValue(&instruction, 21);
  }
  auto full = buildUseTraceSSAFromLotusSVFG(svfg, *module, NativeHistoryMode::Full);
  const auto heap = Event::Allocate | Event::Release;
  for (auto mode : {NativeHistoryMode::DoubleFree, NativeHistoryMode::UseAfterFree,
                    NativeHistoryMode::MemoryLeak, NativeHistoryMode::FileLeak}) {
    auto result = buildUseTraceSSAFromLotusSVFG(svfg, *module, mode);
    EXPECT_TRUE(result.graph.verify());
    EXPECT_LT(result.graph.nodes().size(), full.graph.nodes().size());
    const bool file = mode == NativeHistoryMode::FileLeak;
    EXPECT_EQ(result.graph.resourceCandidates(),
              file ? std::vector<ObjectID>({20, 21}) : std::vector<ObjectID>({10, 11}));
    Event allowed = file ? Event::Open | Event::Close | Event::Escape | Event::Exit : heap;
    if (mode == NativeHistoryMode::UseAfterFree) allowed = allowed | Event::Dereference;
    if (mode == NativeHistoryMode::MemoryLeak) allowed = allowed | Event::Escape | Event::Exit;
    bool unknownAccess = false;
    for (const auto &node : result.graph.nodes()) {
      EXPECT_EQ(node.label.find("@llvm.assume"), std::string::npos);
      EXPECT_EQ(node.label.find("%unrelated"), std::string::npos);
      EXPECT_EQ(node.label.find(file ? "@malloc" : "@fopen"), std::string::npos);
      EXPECT_EQ(static_cast<unsigned>(node.events) & ~static_cast<unsigned>(allowed), 0u);
      for (const auto &effect : node.effects) {
        EXPECT_EQ(static_cast<unsigned>(effect.events) & ~static_cast<unsigned>(allowed), 0u);
        if (effect.events == Event::Dereference && effect.objects.isUnknown()) unknownAccess = true;
      }
    }
    EXPECT_EQ(unknownAccess, mode == NativeHistoryMode::UseAfterFree);
    auto kind = mode == NativeHistoryMode::DoubleFree ? DefectKind::DoubleFree :
                mode == NativeHistoryMode::UseAfterFree ? DefectKind::UseAfterFree :
                mode == NativeHistoryMode::MemoryLeak ? DefectKind::MemoryLeak : DefectKind::FileLeak;
    EXPECT_EQ(DefectDetector(result.graph, 3).scan(kind).status, QueryStatus::Found);
    if (mode == NativeHistoryMode::DoubleFree || mode == NativeHistoryMode::UseAfterFree) {
      auto expected = DefectDetector(full.graph, 3).scan(kind);
      auto actual = DefectDetector(result.graph, 3).scan(kind);
      EXPECT_EQ(actual.findings.size(), expected.findings.size());
      for (const auto &finding : actual.findings)
        EXPECT_EQ(finding.objects, std::vector<ObjectID>({10}));
    }
  }
}

TEST(UseTraceSSANative, UnknownSourceKeepsNamedObjectCandidates) {
  llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"IR(
    declare void @free(i8*)
    define i8 @check(i8* %released, i8* %used) {
      call void @free(i8* %released)
      %value = load i8, i8* %used
      ret i8 %value
    }
  )IR", diagnostic, context);
  ASSERT_TRUE(module);
  lotus::analysis::SVFG svfg;
  svfg.addObjectForValue(module->getFunction("check")->getArg(1), 99);
  auto result = buildUseTraceSSAFromLotusSVFG(svfg, *module, NativeHistoryMode::UseAfterFree);
  EXPECT_EQ(result.graph.resourceCandidates(), std::vector<ObjectID>({99, UnknownResource}));
  auto scan = DefectDetector(result.graph, 3).scan(DefectKind::UseAfterFree);
  ASSERT_EQ(scan.findings.size(), 1u);
  EXPECT_EQ(scan.findings.front().objects, std::vector<ObjectID>({99}));
}

TEST(UseTraceSSANative, MissingPropertySourcesAvoidResourceConstruction) {
  llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"IR(
    define i8 @read_only(i8* %p) { %v = load i8, i8* %p ret i8 %v }
  )IR", diagnostic, context);
  ASSERT_TRUE(module);
  lotus::analysis::SVFG svfg;
  svfg.addObjectForValue(module->getFunction("read_only")->getArg(0), 99);
  for (auto mode : {NativeHistoryMode::DoubleFree, NativeHistoryMode::UseAfterFree,
                    NativeHistoryMode::MemoryLeak, NativeHistoryMode::FileLeak}) {
    auto result = buildUseTraceSSAFromLotusSVFG(svfg, *module, mode);
    EXPECT_TRUE(result.graph.nodes().empty());
    EXPECT_TRUE(result.graph.resourceCandidates().empty());
    EXPECT_FALSE(result.graph.complete());
  }
  EXPECT_FALSE(buildUseTraceSSAFromLotusSVFG(svfg, *module, NativeHistoryMode::Full)
                   .graph.nodes().empty());
}
TEST(UseTraceSSANative, MultipleCallersCannotExchangeReturns) {
  llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
  auto m=llvm::parseAssemblyString(R"IR(
    declare void @free(i8*)
    define void @shared() { ret void }
    define void @one(i8* %p) {
      call void @free(i8* %p)
      call void @shared()
      ret void
    }
    define i8 @two(i8* %p) {
      call void @shared()
      %v = load i8, i8* %p
      ret i8 %v
    }
  )IR",diagnostic,context);
  ASSERT_TRUE(m); lotus::analysis::SVFG svfg;
  for (const char *name : {"one","two"})
    svfg.addObjectForValue(m->getFunction(name)->getArg(0),42);
  auto r=buildUseTraceSSAFromLotusSVFG(svfg,*m,NativeHistoryMode::UseAfterFree);
  auto q=queries::useAfterFree(r.graph); q.memoryObject=42;
  EXPECT_FALSE(QueryEngine(r.graph).run(q).found());
  q.memoryObject.reset();
  EXPECT_FALSE(QueryEngine(r.graph).runObjects({q,ObjectUniverse({42})}).found.any());
  q.context=ContextMode::Insensitive;
  EXPECT_TRUE(QueryEngine(r.graph).runObjects({q,ObjectUniverse({42})}).found.any());
}
TEST(UseTraceSSANative, NoReturnAndRecursion) {
  llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
  auto m=llvm::parseAssemblyString(R"IR(
    declare void @free(i8*)
    define void @stop() { unreachable }
    define i8 @caller(i8* %p) {
      call void @free(i8* %p)
      call void @stop()
      %v = load i8, i8* %p
      ret i8 %v
    }
    define void @recursive(i8* %p, i1 %condition) {
      br i1 %condition, label %base, label %recur
    recur:
      call void @recursive(i8* %p, i1 true)
      ret void
    base:
      call void @free(i8* %p)
      ret void
    }
    define i8 @after_recursion(i8* %p) {
      call void @recursive(i8* %p, i1 false)
      %v = load i8, i8* %p
      ret i8 %v
    }
  )IR",diagnostic,context);
  ASSERT_TRUE(m); lotus::analysis::SVFG svfg;
  svfg.addObjectForValue(m->getFunction("caller")->getArg(0),1);
  for (const char *name : {"recursive","after_recursion"})
    svfg.addObjectForValue(m->getFunction(name)->getArg(0),2);
  for (auto mode : {NativeHistoryMode::Full,NativeHistoryMode::UseAfterFree}) {
    auto r=buildUseTraceSSAFromLotusSVFG(svfg,*m,mode);
    auto q=queries::useAfterFree(r.graph);
    auto batch=QueryEngine(r.graph).runObjects({q,ObjectUniverse({1,2})});
    EXPECT_NE(batch.status(1),QueryStatus::Found);
    EXPECT_EQ(batch.status(2),QueryStatus::Found);
    for (ObjectID object : {1,2}) {
      q.memoryObject=object;
      EXPECT_EQ(batch.status(object),QueryEngine(r.graph).run(q).status);
    }
  }
}
} // namespace
