/**
 * Internal implementation details for FailureDirectedTrimming.
 * Shared types and declarations across FDTrim .cpp sources.
 *
 * Implements the "safety conditions → trimming conditions" pipeline from
 * Ferles et al., ESEC/FSE'17 (paper §4 Static Analysis, §5 Program Instrumentation):
 *   - Safety condition (paper: φ s.t. φ ⇒ wp(s, true)): sufficient condition
 *     for avoiding assertion failure from a program point onward.
 *   - Trimming condition = ¬(safety condition) (paper §5): necessary for failure;
 *     inserted as assume(...) to prune provably safe paths (Theorem 5.1).
 *
 * Core invariants from computeSafetyConditions() (paper Figure 3, rules (1)–(10)):
 *   - BeforeInst[I]: sufficient condition immediately before I.
 *   - PreAfterPhi[BB]: condition at block entry after PHIs (for edge transfer).
 *   - Summary: procedure summary Υ(prc) used at callsites (rule (6), Def. Procedure summary).
 */
#ifndef VERIFICATION_FAILUREDIRECTEDTRIMMING_IMPL_H
#define VERIFICATION_FAILUREDIRECTEDTRIMMING_IMPL_H

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace llvm;

// ---------------------------------------------------------------------------
// LLVM helpers
// ---------------------------------------------------------------------------
inline Function *getDirectCalledFunction(const CallBase &CB) {
  Value *Callee = CB.getCalledOperand();
  if (!Callee)
    return nullptr;
  Callee = Callee->stripPointerCasts();
  return dyn_cast<Function>(Callee);
}

inline Function *getDirectCalledFunctionMatchingType(const CallBase &CB) {
  Function *F = getDirectCalledFunction(CB);
  if (!F)
    return nullptr;
  if (CB.getFunctionType() != F->getFunctionType())
    return nullptr;
  return F;
}

// -----------------------------------------------------------------------------
// Options (defined in Options.cpp)
// -----------------------------------------------------------------------------
extern cl::opt<bool> FDTrimInstrumentCalls;
extern cl::opt<bool> FDTrimInstrumentConditionals;
extern cl::opt<bool> FDTrimInstrumentLoops;
extern cl::opt<unsigned> FDTrimMaxConjuncts;
extern cl::opt<unsigned> FDTrimSummaryIterations;
extern cl::opt<unsigned> FDTrimCFGIterations;
extern cl::opt<std::string> FDTrimQuantElim;
extern cl::opt<unsigned> FDTrimQETTimeoutMs;
extern cl::opt<std::string> FDTrimIntSemantics;
extern cl::opt<std::string> FDTrimDerefMode;
extern cl::opt<std::string> FDTrimAA;
extern cl::opt<bool> FDTrimModelUBOps;

// -----------------------------------------------------------------------------
// Expression language for safety/trimming conditions (paper §4.1: predicates
// p, expressions e; we add drf(α) for heap, Forall/Exists for havoc/negation)
// -----------------------------------------------------------------------------
enum class ExprKind : uint8_t {
  BoolConst,
  IntConst,
  Var,
  BoundVar,
  Not,
  And,
  Or,
  Add,
  Sub,
  Mul,
  BAnd,
  BOr,
  BXor,
  Shl,
  LShr,
  AShr,
  UDiv,
  SDiv,
  URem,
  SRem,
  ICmp,
  Select,
  Deref,
  Cast,
  Gep,
  Forall,
  Exists,
};

struct Expr;
using ExprRef = std::shared_ptr<const Expr>;

struct Expr {
  ExprKind Kind;
  Type *Ty = nullptr;

  bool BoolVal = false;
  APInt IntVal = APInt(1, 0);
  const Value *VarVal = nullptr;
  uint32_t BoundId = 0;

  CmpInst::Predicate Pred = CmpInst::BAD_ICMP_PREDICATE;
  Instruction::CastOps CastOp = Instruction::BitCast;

  Type *DerefValueTy = nullptr;
  Type *GepSourceEltTy = nullptr;
  bool GepInBounds = false;

  std::vector<ExprRef> Args;
};

struct ExprFactory {
  LLVMContext &Ctx;
  explicit ExprFactory(LLVMContext &C) : Ctx(C) {}

  ExprRef boolConst(bool V) const;
  ExprRef intConst(const APInt &V, IntegerType *Ty) const;
  ExprRef var(const Value *V) const;
  ExprRef boundVar(uint32_t Id, Type *Ty) const;
  ExprRef not_(ExprRef A) const;
  ExprRef and_(ArrayRef<ExprRef> Children) const;
  ExprRef or_(ArrayRef<ExprRef> Children) const;
  ExprRef add(ExprRef A, ExprRef B) const;
  ExprRef sub(ExprRef A, ExprRef B) const;
  ExprRef mul(ExprRef A, ExprRef B) const;
  ExprRef band(ExprRef A, ExprRef B) const;
  ExprRef bor(ExprRef A, ExprRef B) const;
  ExprRef bxor(ExprRef A, ExprRef B) const;
  ExprRef shl(ExprRef A, ExprRef B) const;
  ExprRef lshr(ExprRef A, ExprRef B) const;
  ExprRef ashr(ExprRef A, ExprRef B) const;
  ExprRef udiv(ExprRef A, ExprRef B) const;
  ExprRef sdiv(ExprRef A, ExprRef B) const;
  ExprRef urem(ExprRef A, ExprRef B) const;
  ExprRef srem(ExprRef A, ExprRef B) const;
  ExprRef icmp(CmpInst::Predicate P, ExprRef A, ExprRef B) const;
  ExprRef implies(ExprRef Cond, ExprRef Then) const;
  ExprRef select(ExprRef Cond, ExprRef T, ExprRef F) const;
  ExprRef deref(ExprRef Ptr, Type *ValueTy) const;
  ExprRef cast(Instruction::CastOps Op, Type *DstTy, ExprRef Src) const;
  ExprRef gep(Type *SourceEltTy, bool InBounds, ExprRef BasePtr,
              ArrayRef<ExprRef> Indices, Type *ResultTy) const;
  ExprRef forall(uint32_t Id, Type *Ty, ExprRef Body) const;
  ExprRef exists(uint32_t Id, Type *Ty, ExprRef Body) const;
};

struct BoundKey {
  const Instruction *Inst = nullptr;
  uint64_t Tag = 0;
  Type *Ty = nullptr;
};

struct BoundKeyInfo {
  static inline BoundKey getEmptyKey() { return {nullptr, 0, nullptr}; }
  static BoundKey getTombstoneKey();
  static unsigned getHashValue(const BoundKey &K);
  static bool isEqual(const BoundKey &LHS, const BoundKey &RHS);
};

struct BoundVarManager {
  DenseMap<BoundKey, uint32_t, BoundKeyInfo> Ids;
  uint32_t NextId = 1;
  uint32_t getId(const Instruction *I, uint64_t Tag, Type *Ty);
};

struct Subst {
  DenseMap<const Value *, ExprRef> Vars;
  DenseMap<uint32_t, ExprRef> Bound;
};

/// Result of safety-condition analysis for one function (paper Figure 3).
/// Summary = PreAfterPhi[entry], used as callee summary at call sites (rule (10)).
struct FunctionSCResult {
  ExprRef Summary;
  DenseMap<const Instruction *, ExprRef> BeforeInst;
  DenseMap<const BasicBlock *, ExprRef> PreAfterPhi;
};

/// Maps each procedure to its safety-condition summary (paper Υ, Def. Procedure summary).
struct SummaryEnv {
  DenseMap<const Function *, ExprRef> Summaries;
};

/// hasAsrts(f): true iff f or any (transitive) callee contains an assertion (paper Def. Procedure summary).
struct HasAsrtsEnv {
  DenseMap<const Function *, bool> HasAsrts;
};

struct NondetFactory {
  Module &M;
  DenseMap<Type *, FunctionCallee> Cache;
  explicit NondetFactory(Module &Mod) : M(Mod) {}
  FunctionCallee get(Type *Ty);
  Value *nondet(IRBuilder<> &B, Type *Ty);
  Value *nondetBool(IRBuilder<> &B);
};

struct DerefUFFactory {
  Module &M;
  DenseMap<uint64_t, FunctionCallee> Cache;
  explicit DerefUFFactory(Module &Mod) : M(Mod) {}
  FunctionCallee get(Type *RetTy, unsigned AddrSpace);
};

// -----------------------------------------------------------------------------
// Helpers (Options.cpp)
// -----------------------------------------------------------------------------
bool isAssumeNotFunctionName(StringRef Name);
bool isAssumeFunctionName(StringRef Name);
bool isNondetFunctionName(StringRef Name);
bool isAssertFunctionName(StringRef Name);
bool isErrorFunctionName(StringRef Name);
FunctionCallee getVerifierAssume(Module &M);

// -----------------------------------------------------------------------------
// Expression helpers (Expr.cpp)
// -----------------------------------------------------------------------------
std::string exprToString(const ExprRef &E);
bool exprEquals(const ExprRef &A, const ExprRef &B);
bool exprContainsKind(const ExprRef &E, ExprKind K);
bool isNullPtrExpr(const ExprRef &E);
bool mayAliasPtrExpr(const ExprRef &A, const ExprRef &B,
                     lotus::AliasAnalysisWrapper &AA);
ExprRef substitute(const ExprFactory &F, const ExprRef &E, const Subst &S);
void collectDerefPtrs(const ExprRef &E, std::vector<ExprRef> &Out);
void collectDerefNodes(const ExprRef &E, std::vector<ExprRef> &Out);
void collectPointerVars(const ExprRef &E, SmallVectorImpl<const Value *> &Out);
ExprRef negateForTrimming(const ExprFactory &F, const ExprRef &E);
ExprRef boundConjuncts(const ExprFactory &F, const ExprRef &E, unsigned Max);

// -----------------------------------------------------------------------------
// Codegen (Codegen.cpp)
// -----------------------------------------------------------------------------
Value *codegenValue(const ExprFactory &F, const ExprRef &E,
                          IRBuilder<> &B, DenseMap<uint32_t, Value *> &BoundVals,
                          Module &M, NondetFactory &Nondet, DerefUFFactory &DerefUF);
ExprRef eliminateExistsByNondet(const ExprFactory &F, const ExprRef &E,
                                IRBuilder<> &B, Module &M, NondetFactory &Nondet,
                                DenseMap<uint32_t, Value *> &BoundVals);

// -----------------------------------------------------------------------------
// Z3 QE (Z3QE.cpp)
// -----------------------------------------------------------------------------
ExprRef tryEliminateExistsByZ3QE(const ExprFactory &F, Module &M,
                                  const ExprRef &TrimCond);

// -----------------------------------------------------------------------------
// Safety condition inference (SafetyConditions.cpp)
// -----------------------------------------------------------------------------
ExprRef buildValueExpr(const ExprFactory &F, const Value *V);
ExprRef asBoolExpr(const ExprFactory &F, ExprRef E);
ExprRef buildRhsExpr(const ExprFactory &F, const Instruction *I);
ExprRef havocVar(const ExprFactory &F, BoundVarManager &BVM,
                 const Instruction *I, uint64_t Tag, Type *Ty, const Value *V,
                 ExprRef Phi);
ExprRef icmpPtr(const ExprFactory &F, CmpInst::Predicate Pred, ExprRef A, ExprRef B);
ExprRef storeOp(const ExprFactory &F, BoundVarManager &BVM,
                lotus::AliasAnalysisWrapper &AA, const Instruction *CtxI,
                uint64_t TagBase, ExprRef Ptr, Type *StoredValTy, ExprRef NewVal,
                ExprRef Phi);
ExprRef storeTransfer(const ExprFactory &F, BoundVarManager &BVM,
                      lotus::AliasAnalysisWrapper &AA,
                      const Instruction *I, const StoreInst *SI, ExprRef Phi);
ExprRef havocDerefLocation(const ExprFactory &F, BoundVarManager &BVM,
                           lotus::AliasAnalysisWrapper &AA,
                           const Instruction *I, uint64_t Tag, ExprRef Ptr,
                           Type *ValTy, ExprRef Phi);
ExprRef summaryOf(const ExprFactory &F, const SummaryEnv &Env,
                  const HasAsrtsEnv &Has, const Function *Callee,
                  ArrayRef<ExprRef> Actuals);
ExprRef callTransfer(const ExprFactory &F, BoundVarManager &BVM,
                     lotus::AliasAnalysisWrapper &AA, const SummaryEnv &Env,
                     const HasAsrtsEnv &Has, const CallBase *CB, ExprRef Phi);
Instruction *firstNonPhiNonDbg(BasicBlock &B);
ExprRef edgePre(const ExprFactory &F,
                const DenseMap<const BasicBlock *, ExprRef> &PreAfterPhi,
                const BasicBlock *Succ, const BasicBlock *Pred);
FunctionSCResult computeSafetyConditions(
    Function &Fn, const ExprFactory &F, BoundVarManager &BVM,
    lotus::AliasAnalysisWrapper &AA, const SummaryEnv &Env,
    const HasAsrtsEnv &Has);
HasAsrtsEnv computeHasAsrts(Module &M);

// -----------------------------------------------------------------------------
// Clone & wrap (CloneAndWrap.cpp)
// -----------------------------------------------------------------------------
DenseMap<Function *, Function *> cloneSafeFunctions(Module &M,
                                                    FunctionCallee AssumeFn);
bool wrapCallsInOriginalFunctions(Module &M, FunctionCallee AssumeFn,
                                  DenseMap<Function *, Function *> &SafeOf,
                                  NondetFactory &Nondet);

// -----------------------------------------------------------------------------
// Main entry (Pass.cpp)
// -----------------------------------------------------------------------------
bool runFailureDirectedTrimming(Module &M);

#endif
