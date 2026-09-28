#include "IR/UseTraceSSA/SVFGBridge.h"
#include "IR/UseTraceSSA/DefectDetector.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace lotus::usetracessa;
namespace {
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
