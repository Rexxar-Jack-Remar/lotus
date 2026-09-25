#pragma once

#include "Dataflow/NPA/LLVM/AnalysisSupport.h"
#include "Dataflow/NPA/NPA.h"

#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace npa {

template <class D, class Analysis> class InterEngine {
public:
  using Exp = Exp0<D>;
  using E = E0<D>;
  using Val = typename D::value_type;
  using Fact = typename Analysis::FactType;

  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, Val> summaries;
    std::map<BlockKey, Fact> blockEntryFacts;
  };

private:
  struct ApproximationFlags {
    // V1 contract: one analysis run at a time. The engine owns these
    // run-local flags so clients can keep propagation hooks side-effect free
    // after construction without adding synchronization to hot paths.
    bool used_summary_overflow = false;
    bool used_fact_widening = false;
  };

public:
  static std::string getBlockSymbol(const llvm::BasicBlock *BB) {
    std::string s;
    s.reserve(1 + sizeof(BB));
    s.push_back('B');
    s.append(reinterpret_cast<const char *>(&BB), sizeof(BB));
    return s;
  }

  static std::string getFuncSymbol(const llvm::Function *F) {
    std::string s;
    s.reserve(1 + sizeof(F));
    s.push_back('F');
    s.append(reinterpret_cast<const char *>(&F), sizeof(F));
    return s;
  }

  static std::vector<llvm::Function *> getPossibleCallees(
      llvm::Module &M, const llvm::CallBase &Call,
      IndirectCallResolutionMode mode =
          IndirectCallResolutionMode::ClosedWorldTypeCompatible) {
    if (llvm::Function *Direct = Call.getCalledFunction()) {
      if (!Direct->isDeclaration())
        return {Direct};
      return {};
    }

    if (mode == IndirectCallResolutionMode::DeclaredOnlyFallback ||
        mode == IndirectCallResolutionMode::CustomResolverRequired) {
      return {};
    }

    if (auto *CalleeValue = Call.getCalledOperand()) {
      auto *Stripped = CalleeValue->stripPointerCasts();
      if (auto *Direct = llvm::dyn_cast<llvm::Function>(Stripped)) {
        if (!Direct->isDeclaration() &&
            isCallCompatibleWithFunction(Call, *Direct, M.getDataLayout()))
          return {Direct};
        return {};
      }
    }

    std::vector<llvm::Function *> matches;
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      if (isCallCompatibleWithFunction(Call, F, M.getDataLayout()))
        matches.push_back(&F);
    }
    return matches;
  }

  class CalleeCache {
  public:
    CalleeCache(Analysis &analysis, llvm::Module &module,
                IndirectCallResolutionMode mode)
        : analysis_(analysis), module_(module), mode_(mode) {}

    const std::vector<llvm::Function *> &get(const llvm::CallBase &call) {
      auto It = cache_.find(&call);
      if (It != cache_.end())
        return It->second;

      std::vector<llvm::Function *> resolved =
          getPossibleCalleesForAnalysis(analysis_, module_, call, mode_, 0);
      auto Inserted = cache_.emplace(&call, std::move(resolved));
      return Inserted.first->second;
    }

  private:
    Analysis &analysis_;
    llvm::Module &module_;
    IndirectCallResolutionMode mode_;
    std::unordered_map<const llvm::CallBase *, std::vector<llvm::Function *>>
        cache_;
  };

  static bool typesAreCompatible(const llvm::Type *lhs, const llvm::Type *rhs) {
    if (lhs == rhs)
      return true;
    if (!lhs || !rhs)
      return false;
    if (lhs->isPointerTy() || rhs->isPointerTy())
      return false;
    if (lhs->isIntegerTy() && rhs->isIntegerTy())
      return lhs->getIntegerBitWidth() == rhs->getIntegerBitWidth();
    if (lhs->isFloatingPointTy() && rhs->isFloatingPointTy())
      return lhs->getTypeID() == rhs->getTypeID();
    return lhs->isVoidTy() && rhs->isVoidTy();
  }

  static bool isCallCompatibleWithFunction(const llvm::CallBase &Call,
                                           const llvm::Function &F,
                                           const llvm::DataLayout &DL) {
    if (Call.getCallingConv() != F.getCallingConv())
      return false;
    if (auto *CalledOperand = Call.getCalledOperand()) {
      if (!llvm::CastInst::isBitOrNoopPointerCastable(
              F.getType(), CalledOperand->getType(), DL)) {
        return false;
      }
    }
    llvm::FunctionType *calleeTy = F.getFunctionType();
    if (!typesAreCompatible(Call.getType(), calleeTy->getReturnType()))
      return false;
    if (!calleeTy->isVarArg() && Call.arg_size() != calleeTy->getNumParams())
      return false;
    if (calleeTy->isVarArg() && Call.arg_size() < calleeTy->getNumParams())
      return false;
    for (unsigned i = 0; i < calleeTy->getNumParams(); ++i) {
      llvm::Type *argTy = Call.getArgOperand(i)->getType();
      llvm::Type *paramTy = calleeTy->getParamType(i);
      if (argTy->isPointerTy() || paramTy->isPointerTy()) {
        if (!llvm::CastInst::isBitOrNoopPointerCastable(argTy, paramTy, DL))
          return false;
        continue;
      }
      if (!typesAreCompatible(argTy, paramTy)) {
        return false;
      }
    }
    return true;
  }

  template <typename A>
  static auto getMaxPropagationSteps(const A &analysis, int)
      -> decltype(analysis.getMaxPropagationSteps()) {
    return analysis.getMaxPropagationSteps();
  }

  static long getMaxPropagationSteps(const Analysis &, long) { return -1; }

  template <typename A>
  static auto widenFacts(A &analysis, const Fact &oldFact, const Fact &newFact,
                         size_t updates, int)
      -> decltype(analysis.widenFacts(oldFact, newFact, updates)) {
    return analysis.widenFacts(oldFact, newFact, updates);
  }

  static Fact widenFacts(Analysis &, const Fact &, const Fact &newFact, size_t,
                         long) {
    return newFact;
  }

  template <typename A>
  static auto hasCustomWidenFacts(const A &, int)
      -> decltype(std::declval<A &>().widenFacts(std::declval<const Fact &>(),
                                                 std::declval<const Fact &>(),
                                                 std::size_t{}),
                  bool()) {
    return true;
  }

  static bool hasCustomWidenFacts(const Analysis &, long) { return false; }

  template <typename A>
  static auto summaryIsApproximate(const A &analysis, const Val &summary, int)
      -> decltype(analysis.summaryIsApproximate(summary)) {
    return analysis.summaryIsApproximate(summary);
  }

  static bool summaryIsApproximate(const Analysis &, const Val &, long) {
    return false;
  }

  template <typename A>
  static auto applySummaryWithReporting(A &analysis, const Val &summary,
                                        const Fact &fact,
                                        ApproximationFlags &flags, int)
      -> decltype(analysis.applySummary(summary, fact,
                                        &flags.used_summary_overflow)) {
    return analysis.applySummary(summary, fact, &flags.used_summary_overflow);
  }

  static Fact applySummaryWithReporting(Analysis &analysis, const Val &summary,
                                        const Fact &fact, ApproximationFlags &,
                                        long) {
    return analysis.applySummary(summary, fact);
  }

  template <typename A>
  static auto factIsApproximate(const A &analysis, const Fact &fact, int)
      -> decltype(analysis.factIsApproximate(fact)) {
    return analysis.factIsApproximate(fact);
  }

  static bool factIsApproximate(const Analysis &, const Fact &, long) {
    return false;
  }

  template <typename A>
  static auto widenFactsWithReporting(A &analysis, const Fact &oldFact,
                                      const Fact &newFact, size_t updates,
                                      ApproximationFlags &flags, int)
      -> decltype(analysis.widenFacts(oldFact, newFact, updates,
                                      &flags.used_fact_widening)) {
    return analysis.widenFacts(oldFact, newFact, updates,
                               &flags.used_fact_widening);
  }

  template <typename A>
  static auto widenFactsWithReporting(A &analysis, const Fact &oldFact,
                                      const Fact &newFact, size_t updates,
                                      ApproximationFlags &, long)
      -> decltype(analysis.widenFacts(oldFact, newFact, updates)) {
    return analysis.widenFacts(oldFact, newFact, updates);
  }

  static Fact widenFactsWithReporting(Analysis &, const Fact &,
                                      const Fact &newFact, size_t,
                                      ApproximationFlags &, ...) {
    return newFact;
  }

  template <typename A>
  static auto getCallEntryTransfer(A &analysis, const llvm::CallBase &call,
                                   const llvm::Function &callee, int)
      -> decltype(analysis.getCallEntryTransfer(call, callee)) {
    return analysis.getCallEntryTransfer(call, callee);
  }

  static typename D::value_type getCallEntryTransfer(Analysis &,
                                                     const llvm::CallBase &,
                                                     const llvm::Function &,
                                                     long) {
    return D::one();
  }

  template <typename A>
  static auto getCallReturnTransfer(A &analysis, const llvm::CallBase &call,
                                    const llvm::Function &callee, int)
      -> decltype(analysis.getCallReturnTransfer(call, callee)) {
    return analysis.getCallReturnTransfer(call, callee);
  }

  static typename D::value_type getCallReturnTransfer(Analysis &,
                                                      const llvm::CallBase &,
                                                      const llvm::Function &,
                                                      long) {
    return D::one();
  }

  template <typename A>
  static auto getCallToReturnTransfer(A &analysis, const llvm::CallBase &call,
                                      int)
      -> decltype(analysis.getCallToReturnTransfer(call)) {
    return analysis.getCallToReturnTransfer(call);
  }

  static typename D::value_type
  getCallToReturnTransfer(Analysis &, const llvm::CallBase &, long) {
    return D::one();
  }

  template <typename A>
  static auto getPossibleCalleesForAnalysis(A &analysis, llvm::Module &M,
                                            const llvm::CallBase &call,
                                            IndirectCallResolutionMode, int)
      -> decltype(analysis.getPossibleCallees(M, call)) {
    return analysis.getPossibleCallees(M, call);
  }

  static std::vector<llvm::Function *>
  getPossibleCalleesForAnalysis(Analysis &, llvm::Module &M,
                                const llvm::CallBase &call,
                                IndirectCallResolutionMode mode, long) {
    return getPossibleCallees(M, call, mode);
  }

  template <typename A>
  static auto getCallResolutionMode(const A &analysis, int)
      -> decltype(analysis.getCallResolutionMode()) {
    return analysis.getCallResolutionMode();
  }

  static IndirectCallResolutionMode getCallResolutionMode(const Analysis &,
                                                          long) {
    return IndirectCallResolutionMode::ClosedWorldTypeCompatible;
  }

  template <typename A>
  static auto
  getCallFallbackTransfer(A &analysis, const llvm::CallBase &call,
                          const std::vector<llvm::Function *> &callees, int)
      -> decltype(analysis.getCallFallbackTransfer(call, callees)) {
    return analysis.getCallFallbackTransfer(call, callees);
  }

  static typename D::value_type
  getCallFallbackTransfer(Analysis &, const llvm::CallBase &,
                          const std::vector<llvm::Function *> &, long) {
    return D::zero();
  }

  template <typename A>
  static auto getEdgeTransfer(A &analysis, const llvm::Instruction &term,
                              const llvm::BasicBlock &succ, int)
      -> decltype(analysis.getEdgeTransfer(term, succ)) {
    return analysis.getEdgeTransfer(term, succ);
  }

  static typename D::value_type getEdgeTransfer(Analysis &,
                                                const llvm::Instruction &,
                                                const llvm::BasicBlock &,
                                                long) {
    return D::one();
  }

  template <typename A>
  static auto buildBlockEntryExpr(A &analysis, llvm::BasicBlock &BB, E inExpr,
                                  int)
      -> decltype(analysis.buildBlockEntryExpr(BB, inExpr)) {
    return analysis.buildBlockEntryExpr(BB, inExpr);
  }

  static E buildBlockEntryExpr(Analysis &, llvm::BasicBlock &, E inExpr, long) {
    return inExpr;
  }

  struct FunctionArtifacts {
    std::vector<std::pair<Symbol, E>> equations;
    E summaryExpr;
    E fullSummaryExpr;
    std::unordered_map<std::string, E> blockEntryExprs;
    std::unordered_map<std::string, E> blockExitExprs;
    std::unordered_map<const llvm::CallBase *, E> callPrefixExprs;
  };

  struct PreparedFunctionArtifacts {
    llvm::Function *function = nullptr;
    FunctionArtifacts artifacts;
    AnalysisStatus status_delta;
    std::vector<llvm::Function *> discovered_callees;
  };

  struct PreparedCallPropagation {
    llvm::Function *callee = nullptr;
    Symbol callee_symbol;
    Val entry_to_call;
  };

  static bool isZeroExpr(const E &expr) {
    return expr && expr->k == Exp::Term && D::equal(expr->c, D::zero());
  }

  static bool isOneExpr(const E &expr) {
    return expr && expr->k == Exp::Term && D::equal(expr->c, D::one());
  }

  template <class T = D>
  static typename std::enable_if<DomainHasProject<T>::value, E>::type
  makeSummaryEquationExpr(E expr) {
    return Exp::project(std::move(expr));
  }

  template <class T = D>
  static typename std::enable_if<!DomainHasProject<T>::value, E>::type
  makeSummaryEquationExpr(E expr) {
    return expr;
  }

  static E makeCallSummaryExpr(Symbol sym) { return Exp::hole(std::move(sym)); }

  static E combineExpr(E lhs, E rhs) {
    if (!lhs)
      return rhs;
    if (!rhs)
      return lhs;
    if (isZeroExpr(lhs))
      return rhs;
    if (isZeroExpr(rhs))
      return lhs;
    return Exp::ndet(lhs, rhs);
  }

  static E multiplyExpr(E lhs, E rhs) {
    if (!lhs || !rhs)
      return Exp::term(D::zero());
    if (isZeroExpr(lhs) || isZeroExpr(rhs))
      return Exp::term(D::zero());
    if (isOneExpr(lhs))
      return rhs;
    if (isOneExpr(rhs))
      return lhs;
    if (lhs->k == Exp::Term && rhs->k == Exp::Term)
      return Exp::term(D::extend(lhs->c, rhs->c));
    if (lhs->k == Exp::Term)
      return Exp::seq(lhs->c, rhs);
    return Exp::mul(lhs, rhs);
  }

  static E buildBlockBodyExprPrepared(
      Analysis &analysis, llvm::Instruction &I, E currentPath,
      CalleeCache &calleeCache, std::vector<llvm::Function *> &discovered,
      AnalysisStatus &status, IndirectCallResolutionMode callResolutionMode) {
    if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
      if (CI->getCalledFunction() == nullptr)
        ++status.indirect_calls_seen;
      const auto &Callees = calleeCache.get(*CI);
      if (!Callees.empty()) {
        E callBranches = nullptr;
        for (llvm::Function *Callee : Callees) {
          if (!Callee->isDeclaration())
            discovered.push_back(Callee);
          E branch = nullptr;
          if (Callee->isDeclaration()) {
            branch = Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              currentPath);
          } else {
            const Symbol callee_sym = getFuncSymbol(Callee);
            branch = Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                              currentPath);
            branch = multiplyExpr(makeCallSummaryExpr(callee_sym), branch);
            branch = Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              branch);
          }
          callBranches = combineExpr(callBranches, branch);
        }
        Val fallbackTransfer =
            getCallFallbackTransfer(analysis, *CI, Callees, 0);
        if (!D::equal(fallbackTransfer, D::zero())) {
          ++status.fallback_call_edges;
          E fallbackBranch = Exp::seq(fallbackTransfer, currentPath);
          callBranches = callBranches ? Exp::ndet(callBranches, fallbackBranch)
                                      : fallbackBranch;
        }
        currentPath = callBranches;
      } else {
        if (CI->getCalledFunction() == nullptr) {
          ++status.unresolved_indirect_calls;
          if (callResolutionMode ==
              IndirectCallResolutionMode::CustomResolverRequired) {
            status.requires_external_callee_resolver = true;
            status.approximated = true;
          }
        }
        Val fallbackTransfer =
            getCallFallbackTransfer(analysis, *CI, Callees, 0);
        if (!D::equal(fallbackTransfer, D::zero())) {
          ++status.fallback_call_edges;
          currentPath = Exp::seq(fallbackTransfer, currentPath);
        } else {
          currentPath =
              Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
        }
      }
    }
    return analysis.getTransfer(I, currentPath);
  }

  static PreparedFunctionArtifacts
  prepareFunctionArtifacts(llvm::Module &, llvm::Function &F,
                           Analysis &analysis, CalleeCache &calleeCache,
                           IndirectCallResolutionMode callResolutionMode) {
    std::unordered_map<const llvm::BasicBlock *, E> block_bodies;

    PreparedFunctionArtifacts prepared;
    prepared.function = &F;

    for (auto &BB : F) {
      E current_path = Exp::term(D::one());
      for (auto &I : BB) {
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&I))
          prepared.artifacts.callPrefixExprs.emplace(call, current_path);
        current_path = buildBlockBodyExprPrepared(
            analysis, I, current_path, calleeCache, prepared.discovered_callees,
            prepared.status_delta, callResolutionMode);
      }
      block_bodies.emplace(&BB, std::move(current_path));
    }

    E function_summary;
    for (auto &BB : F) {
      E incoming;
      if (&BB == &F.getEntryBlock())
        incoming = Exp::term(D::one());

      for (llvm::BasicBlock *pred : predecessors(&BB)) {
        auto *term = pred->getTerminator();
        Val edge_transfer =
            term ? getEdgeTransfer(analysis, *term, BB, 0) : D::one();
        E branch =
            Exp::seq(std::move(edge_transfer), Exp::hole(getBlockSymbol(pred)));
        incoming = combineExpr(std::move(incoming), std::move(branch));
      }
      if (!incoming)
        incoming = Exp::term(D::zero());

      E entry_expr = buildBlockEntryExpr(analysis, BB, std::move(incoming), 0);
      const Symbol block_symbol = getBlockSymbol(&BB);
      E exit_expr = multiplyExpr(block_bodies.at(&BB), entry_expr);
      prepared.artifacts.equations.emplace_back(block_symbol, exit_expr);
      prepared.artifacts.blockEntryExprs.emplace(block_symbol, entry_expr);
      prepared.artifacts.blockExitExprs.emplace(block_symbol,
                                                Exp::hole(block_symbol));

      auto *term = BB.getTerminator();
      if (!term || term->getNumSuccessors() == 0) {
        function_summary =
            combineExpr(std::move(function_summary), Exp::hole(block_symbol));
      }
    }

    if (!function_summary)
      function_summary = Exp::term(D::zero());
    prepared.artifacts.fullSummaryExpr = function_summary;
    prepared.artifacts.summaryExpr = makeSummaryEquationExpr(function_summary);
    prepared.artifacts.equations.emplace_back(getFuncSymbol(&F),
                                              prepared.artifacts.summaryExpr);
    return prepared;
  }

public:
  static std::vector<llvm::Function *> getEntryFunctions(llvm::Module &M) {
    if (llvm::Function *Main = M.getFunction("main"))
      return {Main};

    std::unordered_set<const llvm::Function *> called;
    std::vector<llvm::Function *> defined;
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      defined.push_back(&F);
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
          if (!CB)
            continue;
          for (llvm::Function *Callee : getPossibleCallees(M, *CB)) {
            if (!Callee->isDeclaration())
              called.insert(Callee);
          }
        }
      }
    }

    std::vector<llvm::Function *> roots;
    for (llvm::Function *F : defined) {
      if (!called.count(F))
        roots.push_back(F);
    }
    return roots.empty() ? defined : roots;
  }

  static Result
  run(llvm::Module &M, Analysis &analysis, bool verbose = false,
      LinearStrategy linearStrategy = LinearStrategy::SCC,
      IndirectCallResolutionMode callResolutionMode =
          IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      NewtonRoundStrategy roundStrategy = NewtonRoundStrategy::Dense) {
    std::vector<std::pair<Symbol, E>> eqns;
    std::set<llvm::Function *> visited;
    std::unordered_map<std::string, FunctionKey> functionSymbols;
    std::unordered_map<std::string, E> fullSummaryExprs;
    std::unordered_map<std::string, E> blockEntryExprs;
    std::unordered_map<std::string, E> blockExitExprs;
    std::unordered_map<const llvm::CallBase *, E> callPrefixExprs;
    CalleeCache calleeCache(analysis, M, callResolutionMode);

    Result res;
    res.status.call_resolution_mode = callResolutionMode;
    res.status.open_world_unsound_mode =
        res.status.call_resolution_mode ==
        IndirectCallResolutionMode::ClosedWorldTypeCompatible;

    std::vector<llvm::Function *> entries = getEntryFunctions(M);
    std::vector<llvm::Function *> frontier(entries.begin(), entries.end());
    visited.insert(frontier.begin(), frontier.end());

    const auto ArtifactStart = std::chrono::steady_clock::now();
    while (!frontier.empty()) {
      std::vector<PreparedFunctionArtifacts> prepared(frontier.size());
      for (std::size_t index = 0; index < frontier.size(); ++index) {
        prepared[index] =
            prepareFunctionArtifacts(M, *frontier[index], analysis, calleeCache,
                                     res.status.call_resolution_mode);
      }

      std::vector<llvm::Function *> next_frontier;
      for (auto &item : prepared) {
        llvm::Function *F = item.function;
        std::string fSym = getFuncSymbol(F);
        functionSymbols[fSym] = {F};
        for (auto &equation : item.artifacts.equations)
          eqns.push_back(std::move(equation));
        fullSummaryExprs.emplace(fSym, item.artifacts.fullSummaryExpr);
        for (auto &blockExpr : item.artifacts.blockEntryExprs)
          blockEntryExprs.emplace(blockExpr.first, blockExpr.second);
        for (auto &blockExpr : item.artifacts.blockExitExprs)
          blockExitExprs.emplace(blockExpr.first, blockExpr.second);
        for (auto &callExpr : item.artifacts.callPrefixExprs)
          callPrefixExprs.emplace(callExpr.first, callExpr.second);
        res.status.indirect_calls_seen += item.status_delta.indirect_calls_seen;
        res.status.unresolved_indirect_calls +=
            item.status_delta.unresolved_indirect_calls;
        res.status.fallback_call_edges += item.status_delta.fallback_call_edges;
        res.status.requires_external_callee_resolver =
            res.status.requires_external_callee_resolver ||
            item.status_delta.requires_external_callee_resolver;
        res.status.approximated =
            res.status.approximated || item.status_delta.approximated;
        for (llvm::Function *callee : item.discovered_callees) {
          if (callee && visited.insert(callee).second)
            next_frontier.push_back(callee);
        }
      }
      frontier.swap(next_frontier);
    }
    res.status.phase_artifact_construction_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      ArtifactStart)
            .count();

    auto rawRes = NPASolver<D>::solve(
        eqns, verbose, -1, linearStrategy, DomainContractMode::Off,
        ConvergencePolicy::DomainDefault, roundStrategy);
    std::unordered_map<Symbol, Val> solvedMap;
    for (auto &p : rawRes.first)
      solvedMap.insert_or_assign(p.first, p.second);

    res.status.summary_solve = rawRes.second;
    res.status.used_bounded_inner_solve =
        rawRes.second.hit_linear_limit || rawRes.second.hit_fixpoint_limit;
    res.status.approximated =
        !rawRes.second.converged || res.status.used_bounded_inner_solve;
    const auto SummaryMaterializationStart = std::chrono::steady_clock::now();
    typename I0<D>::EvaluationContext summaryEvaluationContext;
    for (const auto &entry : functionSymbols) {
      auto exprIt = fullSummaryExprs.find(entry.first);
      if (exprIt == fullSummaryExprs.end())
        continue;
      Val summary = I0<D>::evalCachedWithContext(solvedMap, {}, exprIt->second,
                                                 summaryEvaluationContext);
      if (summaryIsApproximate(analysis, summary, 0))
        res.status.approximated = true;
      res.summaries.insert_or_assign(entry.second, summary);
    }
    res.status.phase_summary_materialization_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      SummaryMaterializationStart)
            .count();
    summaryEvaluationContext.values.clear();
    summaryEvaluationContext.values.rehash(0);

    std::deque<llvm::Function *> worklist2;
    std::set<llvm::Function *> inWorklist2;
    std::unordered_map<std::string, Fact> funcInput;
    std::unordered_map<std::string, size_t> funcUpdates;
    const long maxPropagationSteps = getMaxPropagationSteps(analysis, 0);
    long propagationSteps = 0;
    ApproximationFlags approx_flags;

    for (llvm::Function *Entry : entries) {
      std::string sym = getFuncSymbol(Entry);
      funcInput[sym] = analysis.getEntryValue();
      worklist2.push_back(Entry);
      inWorklist2.insert(Entry);
    }

    const auto PropagationStart = std::chrono::steady_clock::now();

    // All algebraic path summaries below depend only on the solved procedure
    // summaries, not on the changing entry facts. Materialize them once and
    // share evaluation state across roots that reuse expression prefixes.
    typename I0<D>::EvaluationContext propagationEvaluationContext;
    std::unordered_map<std::string, Val> blockExitSummaries;
    blockExitSummaries.reserve(blockExitExprs.size());
    for (const auto &entry : blockExitExprs) {
      blockExitSummaries.emplace(
          entry.first,
          I0<D>::evalCachedWithContext(solvedMap, {}, entry.second,
                                       propagationEvaluationContext));
    }

    std::unordered_map<const llvm::BasicBlock *, Val> blockEntrySummaries;
    blockEntrySummaries.reserve(blockEntryExprs.size());
    for (llvm::Function *function : visited) {
      if (!function || function->isDeclaration())
        continue;
      for (auto &block : *function) {
        const std::string blockSymbol = getBlockSymbol(&block);
        auto entryExpression = blockEntryExprs.find(blockSymbol);
        if (entryExpression == blockEntryExprs.end())
          continue;
        Val summary = D::zero();
        if (llvm::isa<llvm::PHINode>(block.begin())) {
          E blockExpression =
              buildBlockEntryExpr(analysis, block, Exp::term(D::zero()), 0);
          std::unordered_map<Symbol, Val> environment = solvedMap;
          for (auto *predecessor : predecessors(&block)) {
            const std::string predecessorSymbol = getBlockSymbol(predecessor);
            auto predecessorSummary =
                blockExitSummaries.find(predecessorSymbol);
            if (predecessorSummary != blockExitSummaries.end())
              environment.insert_or_assign(predecessorSymbol,
                                           predecessorSummary->second);
          }
          summary = I0<D>::eval(false, environment, blockExpression);
        } else {
          summary = I0<D>::evalCachedWithContext(solvedMap, {},
                                                 entryExpression->second,
                                                 propagationEvaluationContext);
        }
        blockEntrySummaries.emplace(&block, std::move(summary));
      }
    }

    std::unordered_map<const llvm::BasicBlock *,
                       std::vector<PreparedCallPropagation>>
        preparedCallsByBlock;
    preparedCallsByBlock.reserve(blockEntrySummaries.size());
    for (const auto &entry : callPrefixExprs) {
      const llvm::CallBase *call = entry.first;
      auto blockSummary = blockEntrySummaries.find(call->getParent());
      if (blockSummary == blockEntrySummaries.end())
        continue;
      Val prefix = I0<D>::evalCachedWithContext(solvedMap, {}, entry.second,
                                                propagationEvaluationContext);
      auto &edges = preparedCallsByBlock[call->getParent()];
      for (llvm::Function *callee : calleeCache.get(*call)) {
        if (!callee || callee->isDeclaration())
          continue;
        Val callEntry = D::extend(
            getCallEntryTransfer(analysis, *call, *callee, 0), prefix);
        Val totalToCall = D::extend(callEntry, blockSummary->second);
        edges.push_back(
            {callee, getFuncSymbol(callee), std::move(totalToCall)});
      }
    }
    propagationEvaluationContext.values.clear();
    propagationEvaluationContext.values.rehash(0);

    while (!worklist2.empty()) {
      if (maxPropagationSteps >= 0 &&
          propagationSteps++ >= maxPropagationSteps) {
        res.status.propagation_hit_limit = true;
        res.status.propagation_converged = false;
        res.status.approximated = true;
        if (verbose)
          std::cerr << "[interproc-fwd] hit max propagation steps="
                    << maxPropagationSteps << "\n";
        break;
      }
      llvm::Function *F = worklist2.front();
      worklist2.pop_front();
      inWorklist2.erase(F);

      std::string fSym = getFuncSymbol(F);
      Fact inputVal = funcInput[fSym];

      for (auto &BB : *F) {
        auto blockSummary = blockEntrySummaries.find(&BB);
        if (blockSummary == blockEntrySummaries.end())
          continue;
        auto blockEntryFact = applySummaryWithReporting(
            analysis, blockSummary->second, inputVal, approx_flags, 0);
        if (factIsApproximate(analysis, blockEntryFact, 0))
          res.status.approximated = true;
        res.blockEntryFacts.insert_or_assign(BlockKey{&BB},
                                             std::move(blockEntryFact));

        auto preparedCalls = preparedCallsByBlock.find(&BB);
        if (preparedCalls != preparedCallsByBlock.end()) {
          for (const PreparedCallPropagation &edge : preparedCalls->second) {
            auto factAtCall = applySummaryWithReporting(
                analysis, edge.entry_to_call, inputVal, approx_flags, 0);
            auto existing = funcInput.find(edge.callee_symbol);
            if (existing == funcInput.end()) {
              funcInput.emplace(edge.callee_symbol, std::move(factAtCall));
              if (inWorklist2.insert(edge.callee).second)
                worklist2.push_back(edge.callee);
              continue;
            }

            Fact joined = analysis.joinFacts(existing->second, factAtCall);
            if (analysis.factsEqual(existing->second, joined))
              continue;
            size_t updateCount = ++funcUpdates[edge.callee_symbol];
            Fact widened =
                widenFactsWithReporting(analysis, existing->second, joined,
                                        updateCount, approx_flags, 0);
            if (hasCustomWidenFacts(analysis, 0))
              res.status.approximated = true;
            if (analysis.factsEqual(existing->second, widened))
              continue;
            existing->second = std::move(widened);
            if (inWorklist2.insert(edge.callee).second)
              worklist2.push_back(edge.callee);
          }
        }
      }
    }
    res.status.phase_propagation_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      PropagationStart)
            .count();
    if (approx_flags.used_summary_overflow) {
      res.status.used_summary_overflow = true;
      res.status.approximated = true;
    }
    if (approx_flags.used_fact_widening) {
      res.status.used_fact_widening = true;
      res.status.approximated = true;
    }
    res.status.propagation_steps = propagationSteps;
    res.status.overall_hit_limit =
        res.status.summary_solve.hit_limit || res.status.propagation_hit_limit;
    res.status.overall_converged = res.status.summary_solve.converged &&
                                   res.status.propagation_converged &&
                                   !res.status.used_summary_overflow &&
                                   !res.status.used_fact_widening;
    return res;
  }
};

} // namespace npa

