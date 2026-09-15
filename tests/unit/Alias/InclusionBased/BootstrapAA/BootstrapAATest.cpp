#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"

#include <stdexcept>

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using lotus::bootstrap::BootstrapAA;
using lotus::bootstrap::Point;
using lotus::bootstrap::PointsToSet;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(std::string(__FILE__) + ":" +                   \
                               std::to_string(__LINE__) + ": " #x);            \
  } while (false)
namespace {
struct Fixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit Fixture(const char *ir) {
    llvm::SMDiagnostic diagnostic;
    module = llvm::parseAssemblyString(ir, diagnostic, context);
    if (!module) {
      diagnostic.print("BootstrapAALLVMTest", llvm::errs());
      throw std::runtime_error("failed to parse test IR");
    }
  }
  const llvm::Instruction &instruction(const char *function,
                                       const char *name) const {
    const llvm::Function *f = module->getFunction(function);
    CHECK(f);
    for (const llvm::BasicBlock &block : *f)
      for (const llvm::Instruction &i : block)
        if (i.getName() == name)
          return i;
    throw std::runtime_error(std::string("missing instruction ") + name);
  }
};
lotus::bootstrap::Options options() {
  lotus::bootstrap::Options result;
  result.andersen_threshold = 1;
  return result;
}
void callsAndStackStores() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    define i8* @identity(i8* %p) { ret i8* %p }
    define i32 @main() {
      %s = alloca i8*
      store i8* @a, i8** %s
      %old = load i8*, i8** %s
      store i8* @b, i8** %s
      %new = load i8*, i8** %s
      %x = call i8* @identity(i8* @a)
      %y = call i8* @identity(i8* @b)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  aa.precomputeAll();
  auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto &old = f.instruction("main", "old"),
             &now = f.instruction("main", "new");
  CHECK(aa.pointsTo(old, old, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(now, now, {}, Point::After).points_to == PointsToSet(b));
  const auto &x = f.instruction("main", "x"), &y = f.instruction("main", "y");
  CHECK(aa.pointsTo(x, x, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(y, y, {}, Point::After).points_to == PointsToSet(b));
  const auto *identity = f.module->getFunction("identity");
  const auto &ret = identity->getEntryBlock().back();
  CHECK(aa.pointsTo(*identity->getArg(0), ret, {llvm::cast<llvm::CallBase>(&x)})
            .points_to == PointsToSet(a));
  CHECK(aa.pointsTo(*identity->getArg(0), ret, {llvm::cast<llvm::CallBase>(&y)})
            .points_to == PointsToSet(b));
  CHECK(!aa.mayAlias(old, now, x));
  CHECK(aa.objectInfo(aa.objectId(f.instruction("main", "s"))).singleton);
  CHECK(!aa.hierarchy().successors.empty());
}
void globalsAndExternalHavoc() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    @slot = global i8* @a
    declare void @opaque()
    define i32 @main() {
      %old = load i8*, i8** @slot
      store i8* @b, i8** @slot
      %new = load i8*, i8** @slot
      call void @opaque()
      %unknown = load i8*, i8** @slot
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &old = f.instruction("main", "old"),
             &now = f.instruction("main", "new");
  CHECK(aa.pointsTo(old, old, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("a"))));
  CHECK(aa.pointsTo(now, now, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("b"))));
  const auto &unknown = f.instruction("main", "unknown");
  CHECK(aa.pointsTo(unknown, unknown, {}, Point::After).points_to.isTop());
}
void partialStoresAndFreeze() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @slot = global i8* @a
    define i32 @main() {
      %byte = bitcast i8** @slot to i8*
      store i8 7, i8* %byte
      %p = load i8*, i8** @slot
      %frozen = freeze i8* undef
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  for (const char *name : {"p", "frozen"}) {
    const auto &i = f.instruction("main", name);
    CHECK(aa.pointsTo(i, i, {}, Point::After).points_to.isTop());
  }
}
void phiAndRecursion() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    define i8* @rec(i1 %c, i8* %p) {
      %s = alloca i8*
      br i1 %c, label %base, label %step
    base:
      ret i8* %p
    step:
      %r = call i8* @rec(i1 %c, i8* %p)
      ret i8* %r
    }
    define i32 @main(i1 %c) {
      br i1 %c, label %left, label %right
    left:
      br label %merge
    right:
      br label %merge
    merge:
      %p = phi i8* [ @a, %left ], [ @b, %right ]
      %r = call i8* @rec(i1 %c, i8* %p)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  PointsToSet expected(aa.objectId(*f.module->getNamedGlobal("a")));
  expected.insert(aa.objectId(*f.module->getNamedGlobal("b")));
  const auto &r = f.instruction("main", "r");
  CHECK(aa.pointsTo(r, r, {}, Point::After).points_to == expected);
  CHECK(!aa.objectInfo(aa.objectId(f.instruction("rec", "s"))).singleton);
}
void byvalCopies() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    %T = type { i8* }
    define void @copy(%T* byval(%T) %arg) {
      %p = bitcast %T* %arg to i8*
      ret void
    }
    define i32 @main() {
      %s = alloca %T
      call void @copy(%T* byval(%T) %s)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &p = f.instruction("copy", "p");
  const auto *arg = f.module->getFunction("copy")->getArg(0);
  auto result = aa.pointsToAllContexts(p, p, Point::After);
  CHECK(result.points_to == PointsToSet(aa.objectId(*arg)));
  CHECK(!result.points_to.contains(aa.objectId(f.instruction("main", "s"))));
}
void allocationsCanFail() {
  Fixture f(R"IR(
    declare i8* @malloc(i64)
    define i32 @main() {
      %p = call i8* @malloc(i64 8)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &p = f.instruction("main", "p");
  auto result = aa.pointsTo(p, p, {}, Point::After);
  CHECK(!result.points_to.isTop());
  CHECK(result.points_to.contains(lotus::bootstrap::NULL_OBJECT));
  CHECK(result.points_to.contains(aa.objectId(p)));
  CHECK(!aa.objectInfo(aa.objectId(p)).singleton);
}
void expandedAllocationModels() {
  Fixture f(R"IR(
    @a = global i8 0
    declare i8* @realloc(i8*, i64)
    declare i8* @aligned_alloc(i64, i64)
    declare i8* @_Znwm(i64)
    define i32 @main() {
      %resized = call i8* @realloc(i8* @a, i64 16)
      %aligned = call i8* @aligned_alloc(i64 16, i64 32)
      %created = call i8* @_Znwm(i64 8)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &resized = f.instruction("main", "resized");
  PointsToSet resizedExpected(aa.objectId(*f.module->getNamedGlobal("a")));
  resizedExpected.insert(aa.objectId(resized));
  resizedExpected.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(resized, resized, {}, Point::After).points_to ==
        resizedExpected);

  const auto &aligned = f.instruction("main", "aligned");
  PointsToSet alignedExpected(aa.objectId(aligned));
  alignedExpected.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(aligned, aligned, {}, Point::After).points_to ==
        alignedExpected);

  const auto &created = f.instruction("main", "created");
  CHECK(aa.pointsTo(created, created, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(created)));
}
void zeroLengthMemoryIntrinsic() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @slot = global i8* @a
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1 immarg)
    define i32 @main() {
      %bytes = bitcast i8** @slot to i8*
      call void @llvm.memset.p0i8.i64(i8* %bytes, i8 0, i64 0, i1 false)
      %after = load i8*, i8** @slot
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &after = f.instruction("main", "after");
  CHECK(aa.pointsTo(after, after, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("a"))));
}
void globalAliasDirectCallee() {
  Fixture f(R"IR(
    @a = global i8 0
    @identity_alias = alias i8* (i8*), i8* (i8*)* @identity
    define i8* @identity(i8* %p) { ret i8* %p }
    define i32 @main() {
      %result = call i8* @identity_alias(i8* @a)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &result = f.instruction("main", "result");
  CHECK(aa.pointsTo(result, result, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("a"))));
}
void readOnlyInteriorPointerModel() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @slot = global i8* @a
    declare i8* @strchr(i8*, i32)
    define i32 @main() {
      %found = call i8* @strchr(i8* @a, i32 0)
      %after = load i8*, i8** @slot
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto &found = f.instruction("main", "found");
  PointsToSet expected(a);
  expected.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(found, found, {}, Point::After).points_to == expected);
  const auto &after = f.instruction("main", "after");
  CHECK(aa.pointsTo(after, after, {}, Point::After).points_to ==
        PointsToSet(a));
}
void vectorPointerOperationsAreConservative() {
  Fixture f(R"IR(
    @a = global i8 0
    define i32 @main() {
      %addresses = getelementptr i8, i8* @a, <2 x i64> <i64 0, i64 1>
      %element = extractelement <2 x i8*> %addresses, i32 0
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &element = f.instruction("main", "element");
  CHECK(aa.pointsTo(element, element, {}, Point::After).points_to.isTop());
}
} // namespace
TEST(BootstrapAATest, CallsAndStackStores) { callsAndStackStores(); }
TEST(BootstrapAATest, GlobalsAndExternalHavoc) { globalsAndExternalHavoc(); }
TEST(BootstrapAATest, PartialStoresAndFreeze) { partialStoresAndFreeze(); }
TEST(BootstrapAATest, PhiAndRecursion) { phiAndRecursion(); }
TEST(BootstrapAATest, ByvalCopies) { byvalCopies(); }
TEST(BootstrapAATest, AllocationsCanFail) { allocationsCanFail(); }
TEST(BootstrapAATest, ExpandedAllocationModels) { expandedAllocationModels(); }
TEST(BootstrapAATest, ZeroLengthMemoryIntrinsic) {
  zeroLengthMemoryIntrinsic();
}
TEST(BootstrapAATest, GlobalAliasDirectCallee) { globalAliasDirectCallee(); }
TEST(BootstrapAATest, ReadOnlyInteriorPointerModel) {
  readOnlyInteriorPointerModel();
}
TEST(BootstrapAATest, VectorPointerOperationsAreConservative) {
  vectorPointerOperationsAreConservative();
}
