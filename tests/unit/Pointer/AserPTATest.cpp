//
// Updated for modern LLVM compatibility
//
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/AserPTA/PointerAnalysis/Program/CallSite.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PartialUpdateSolver.h"
#include "Alias/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/InsertGlobalCtorCallPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"

using namespace llvm;
using namespace aser;

#define CHECK_NO_ALIAS_FUN "__aser_no_alias__"
#define CHECK_ALIAS_FUN "__aser_alias__"

using Model = DefaultLangModel<NoCtx, FSMemModel<NoCtx>>;
using Solver = PartialUpdateSolver<Model>;
// using Solver = DeepPropagation<Model>;

cl::opt<std::string> TestIR(cl::Positional, cl::desc("path to input bitcode file"));

namespace {

std::unique_ptr<Module> parseAssembly(LLVMContext &ctx, const char *ir) {
  SMDiagnostic err;
  auto M = parseAssemblyString(ir, err, ctx);
  if (!M)
    err.print("AserPTATest", errs());
  return M;
}

void addAserPTAPasses(llvm::legacy::PassManager &passes) {
  passes.add(new CanonicalizeGEPPass());
  passes.add(new LoweringMemCpyPass());
  passes.add(new RemoveExceptionHandlerPass());
  passes.add(new InsertGlobalCtorCallPass());
  passes.add(new PointerAnalysisPass<Solver>());
}

class AserMarkerCallSite {
private:
    aser::CallSite CS;

    inline bool isFunNameEqualsTo(llvm::StringRef funName) const {
        if (CS.isCallOrInvoke() && llvm::isa<llvm::CallInst>(CS.getInstruction())) {
            if (const auto *fun = llvm::dyn_cast<llvm::Function>(CS.getCalledValue())) {
                return fun->getName().contains(funName);
            }
        }
        return false;
    }

public:
    explicit AserMarkerCallSite(llvm::Instruction *II) : CS(II) {}

    [[nodiscard]] inline bool isNoAliasCheck() const { return isFunNameEqualsTo(CHECK_NO_ALIAS_FUN); }

    [[nodiscard]] inline bool isAliasCheck() const { return isFunNameEqualsTo(CHECK_ALIAS_FUN); }

    [[nodiscard]] inline const llvm::Value* getArgOperand(unsigned int i) const {
        return CS.getArgOperand(i);
    }
};

template <typename Solver>
class PTAVerificationPass : public llvm::ModulePass {
public:
    using ctx = NoCtx;

    static char ID;
    PTAVerificationPass() : llvm::ModulePass(ID) {
        static_assert(std::is_same<typename Solver::ctx, NoCtx>::value, "Only support context insensitive");
    }

    void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
        AU.addRequired<PointerAnalysisPass<Solver>>();
        AU.setPreservesAll();  // does not transform the LLVM module
    }

    bool runOnModule(llvm::Module &M) override {
        this->getAnalysis<PointerAnalysisPass<Solver>>().analyze(&M, "main");

        auto &pta = *(this->getAnalysis<PointerAnalysisPass<Solver>>().getPTA());

        for (auto &F : M) {
            for (auto &BB : F) {
                for (auto &I : BB) {
                    AserMarkerCallSite CS(&I);
                    if (CS.isNoAliasCheck()) {
                        const auto *ptr1 = CS.getArgOperand(0);
                        const auto *ptr2 = CS.getArgOperand(1);

                        EXPECT_FALSE(pta.alias(nullptr, ptr1, nullptr, ptr2));

                    } else if (CS.isAliasCheck()) {
                        const auto *ptr1 = CS.getArgOperand(0);
                        const auto *ptr2 = CS.getArgOperand(1);

                        EXPECT_TRUE(pta.alias(nullptr, ptr1, nullptr, ptr2));
                    }
                }
            }
        }

        return false;
    }
};

template <typename PTA>
char PTAVerificationPass<PTA>::ID = 0;

// Query pass: expects two distinct allocas in main to be no-alias.
class AserPTAQueryNoAliasPass : public llvm::ModulePass {
public:
  static char ID;
  AserPTAQueryNoAliasPass() : llvm::ModulePass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<PointerAnalysisPass<Solver>>();
    AU.setPreservesAll();
  }

  bool runOnModule(llvm::Module &M) override {
    getAnalysis<PointerAnalysisPass<Solver>>().analyze(&M, "main");
    Solver &pta = *getAnalysis<PointerAnalysisPass<Solver>>().getPTA();

    llvm::Function *mainFn = M.getFunction("main");
    if (!mainFn) { ADD_FAILURE() << "main not found"; return false; }

    const llvm::Value *a1 = nullptr, *a2 = nullptr;
    for (auto &BB : *mainFn) {
      for (auto &I : BB) {
        if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
          if (!a1) a1 = AI;
          else if (!a2) { a2 = AI; break; }
        }
      }
      if (a2) break;
    }
    if (!a1 || !a2) { ADD_FAILURE() << "expected two allocas in main"; return false; }
    EXPECT_FALSE(pta.alias(nullptr, a1, nullptr, a2));
    return false;  // no IR change
  }
};
char AserPTAQueryNoAliasPass::ID = 0;

// Query pass: expects store x to p, load q from p; x and q should alias.
class AserPTAQueryAliasPass : public llvm::ModulePass {
public:
  static char ID;
  AserPTAQueryAliasPass() : llvm::ModulePass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<PointerAnalysisPass<Solver>>();
    AU.setPreservesAll();
  }

  bool runOnModule(llvm::Module &M) override {
    getAnalysis<PointerAnalysisPass<Solver>>().analyze(&M, "main");
    Solver &pta = *getAnalysis<PointerAnalysisPass<Solver>>().getPTA();

    llvm::Function *mainFn = M.getFunction("main");
    if (!mainFn) { ADD_FAILURE() << "main not found"; return false; }

    const llvm::AllocaInst *xAlloca = nullptr;
    const llvm::LoadInst *qLoad = nullptr;
    for (auto &BB : *mainFn) {
      for (auto &I : BB) {
        if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
          if (AI->getAllocatedType()->isIntegerTy(32))
            xAlloca = AI;
        }
        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
          if (LI->getType()->isPointerTy())
            qLoad = LI;
        }
      }
    }
    if (!xAlloca || !qLoad) { ADD_FAILURE() << "expected alloca and load in main"; return false; }
    EXPECT_TRUE(pta.alias(nullptr, xAlloca, nullptr, qLoad));
    return false;  // no IR change
  }
};
char AserPTAQueryAliasPass::ID = 0;

// Pass registration for optional use by opt; unit tests run the pass directly.
// static llvm::RegisterPass<PointerAnalysisPass<Solver>> PAP("Pointer Analysis Wrapper Pass",
//                                                            "Pointer Analysis Wrapper Pass", true, true);

}  // namespace

TEST(AserPTA, NoAliasTwoAllocas) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses(passes);
  passes.add(new AserPTAQueryNoAliasPass());
  passes.run(*module);
}

TEST(AserPTA, AliasStoreLoad) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses(passes);
  passes.add(new AserPTAQueryAliasPass());
  passes.run(*module);
}

TEST(PTACorrectness, pta_correctness) {
    SMDiagnostic Err;

    auto context = std::make_unique<LLVMContext>();
    auto module = parseIRFile(TestIR, Err, *context);

    if (!module) {
        ASSERT_FALSE(false);
        return;
    }

    llvm::legacy::PassManager passes;

    passes.add(new CanonicalizeGEPPass());
    passes.add(new LoweringMemCpyPass());
    passes.add(new RemoveExceptionHandlerPass());

    passes.add(new InsertGlobalCtorCallPass());
    passes.add(new PointerAnalysisPass<Solver>());
    passes.add(new PTAVerificationPass<Solver>());

    passes.run(*module);
}
