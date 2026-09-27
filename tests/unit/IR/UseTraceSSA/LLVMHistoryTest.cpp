#include "IR/UseTraceSSA/HistoryPass.h"
#include "IR/UseTraceSSA/LLVMFlow.h"
#include "IR/UseTraceSSA/Models.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>
#include <stdexcept>

using namespace lotus::usetracessa;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
static std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &ctx, const char *text) {
  llvm::SMDiagnostic error; auto module = llvm::parseAssemblyString(text,error,ctx);
  if (!module) { error.print("UseTraceSSA LLVM test",llvm::errs()); throw std::runtime_error("parse failed"); }
  return module;
}
static llvm::Instruction &instruction(llvm::Function &f, const char *name) {
  for (auto &b : f) for (auto &i : b) if (i.getName() == name) return i;
  throw std::runtime_error("missing named instruction");
}
static std::string text(const llvm::Module &m) {
  std::string result; llvm::raw_string_ostream out(result); m.print(out,nullptr); out.flush(); return result;
}
static void guardsAndUses() {
  llvm::LLVMContext ctx;
  auto m=parse(ctx,R"IR(
    declare void @free(i8*)
    define i8 @paper(i8* %p) {
    entry:
      %check = icmp ne i8* %p, null
      br i1 %check, label %nonnull, label %join
    nonnull:
      %byte = load i8, i8* %p
      call void @free(i8* %p)
      br label %join
    join:
      %y = phi i8 [ 42, %entry ], [ %byte, %nonnull ]
      call void @free(i8* %p)
      ret i8 %y
    }
  )IR");
  std::string original=text(*m); auto &f=*m->getFunction("paper");
  TraceFlowGraph graph; auto h=appendLLVMHistory(graph,0,f);
  CHECK(h.graph().verify()); CHECK(h.nullGuards().size()==2);
  auto &load=instruction(f,"byte"); auto uses=h.uses(load.getOperandUse(0)); CHECK(uses.size()==1);
  CHECK(h.siteID(load)!=InvalidID);
  Query q=queries::uncheckedUse({graph.version(0,h.definition(*f.getArg(0)))},
                                 {graph.version(0,uses.front().history.after)});
  CHECK(QueryEngine(graph).allPathsHitTraps(q).status==CoverageStatus::AllPathsTrapped);
  CHECK(text(*m)==original); // Analysis must not rewrite program IR.
  auto declaration=LLVMHistoryBuilder::build(*m->getFunction("free")); CHECK(declaration.graph().nodes().empty());
}
static void repeatedAndScalar() {
  llvm::LLVMContext ctx;
  auto m=parse(ctx,R"IR(
    define i32 @sum(i32 %x) {
    entry:
      %twice = add i32 %x, %x
      %plus = add i32 %twice, 1
      ret i32 %plus
    }
  )IR");
  auto &f=*m->getFunction("sum"); auto &twice=instruction(f,"twice");
  TraceFlowGraph graph; auto h=appendLLVMHistory(graph,0,f);
  auto a=h.uses(twice.getOperandUse(0)),b=h.uses(twice.getOperandUse(1));
  CHECK(a.size()==1 && b.size()==1); CHECK(a[0].history.after==b[0].history.after);
  appendLLVMScalarTransfers(graph,0,h);
  Query q; q.sources={graph.version(0,h.definition(*f.getArg(0)))};
  q.sinks={graph.version(0,h.definition(instruction(f,"plus")))};
  CHECK(QueryEngine(graph).run(q).found());
  auto pointers=LLVMHistoryBuilder::build(f,{true,false,false}); CHECK(pointers.graph().nodes().empty());
  UseTraceSSALegacyPass pass; CHECK(!pass.runOnFunction(f)); CHECK(pass.getResult().graph().verify());
  pass.releaseMemory();
}
static void parallelPhi() {
  llvm::LLVMContext ctx;
  auto m=parse(ctx,R"IR(
    define i32 @parallel(i32 %x, i32 %tag) {
    entry:
      switch i32 %tag, label %join [ i32 0, label %join ]
    join:
      %p = phi i32 [ %x, %entry ], [ %x, %entry ]
      ret i32 %p
    }
  )IR");
  auto &f=*m->getFunction("parallel"); auto &phi=instruction(f,"p");
  auto h=LLVMHistoryBuilder::build(f);
  auto a=h.uses(phi.getOperandUse(0)),b=h.uses(phi.getOperandUse(1));
  CHECK(a.size()==1 && b.size()==1); CHECK(a[0].edge!=b[0].edge);
  CHECK(h.graph().verify());
}
static void invokeResult() {
  llvm::LLVMContext ctx;
  auto m=parse(ctx,R"IR(
    declare i8* @may_throw()
    declare i32 @personality(...)
    define i8* @inv() personality i32 (...)* @personality {
    entry:
      %p = invoke i8* @may_throw() to label %normal unwind label %cleanup
    normal:
      %q = phi i8* [ %p, %entry ]
      ret i8* %q
    cleanup:
      %lp = landingpad { i8*, i32 } cleanup
      ret i8* null
    }
  )IR");
  auto &f=*m->getFunction("inv"); auto h=LLVMHistoryBuilder::build(f);
  auto def=h.definition(instruction(f,"p"));
  CHECK(h.graph().node(def).region==h.graph().edgeRegion(0)); CHECK(h.graph().verify());
}
static void addressIsNotContent() {
  llvm::LLVMContext ctx;
  auto m=parse(ctx,R"IR(
    define i32 @load(i32* %p) {
      %v = load i32, i32* %p
      %r = add i32 %v, 1
      ret i32 %r
    }
  )IR");
  auto &f=*m->getFunction("load"); TraceFlowGraph g;auto h=appendLLVMHistory(g,0,f);
  appendLLVMScalarTransfers(g,0,h);
  Query q;q.sources={g.version(0,h.definition(*f.getArg(0)))};
  q.sinks={g.version(0,h.definition(instruction(f,"r")))};
  CHECK(QueryEngine(g).run(q).status==QueryStatus::NotFound);
}
int main() {
  try { guardsAndUses(); repeatedAndScalar(); parallelPhi(); invokeResult(); addressIsNotContent(); }
  catch (const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
  return 0;
}
