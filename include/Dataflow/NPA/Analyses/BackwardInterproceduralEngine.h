#ifndef NPA_BACKWARD_INTERPROCEDURAL_ENGINE_H
#define NPA_BACKWARD_INTERPROCEDURAL_ENGINE_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/NPA.h"

#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace npa {

template <class D, class Analysis> class BackwardInterproceduralEngine {
public:
  using Exp = Exp0<D>;
  using E = E0<D>;
  using Val = typename D::value_type;
  using Fact = typename Analysis::FactType;

  struct Result {
    std::map<FunctionKey, Val> summaries;
    std::map<BlockKey, Fact> blockEntryFacts;
  };

private:
  template <typename A>
  static auto getMaxPropagationSteps(const A &analysis, int)
      -> decltype(analysis.getMaxPropagationSteps()) {
    return analysis.getMaxPropagationSteps();
  }

  static long getMaxPropagationSteps(const Analysis &, long) { return 100000; }

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

public:
  static Result run(llvm::Module &M, Analysis &analysis, bool verbose = false) {
    std::vector<std::pair<Symbol, E>> eqns;
    std::deque<llvm::Function *> worklist;
    std::set<llvm::Function *> visited;
    std::unordered_map<std::string, FunctionKey> functionSymbols;
    std::unordered_map<std::string, BlockKey> blockSymbols;

    std::vector<llvm::Function *> entries =
        InterproceduralEngine<D, Analysis>::getEntryFunctions(M);
    for (llvm::Function *Entry : entries) {
      worklist.push_back(Entry);
      visited.insert(Entry);
    }

    while (!worklist.empty()) {
      llvm::Function *F = worklist.front();
      worklist.pop_front();

      std::string fSym = InterproceduralEngine<D, Analysis>::getFuncSymbol(F);
      functionSymbols[fSym] = {F};

      llvm::BasicBlock *Entry = F->empty() ? nullptr : &F->getEntryBlock();
      for (auto &BB : *F) {
        std::string bSym =
            InterproceduralEngine<D, Analysis>::getBlockSymbol(&BB);
        blockSymbols[bSym] = {&BB};

        E outExpr = nullptr;
        auto *Term = BB.getTerminator();
        if (Term) {
          if (Term->getNumSuccessors() == 0) {
            outExpr = Exp::term(D::one());
          } else {
            for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
              auto *Succ = Term->getSuccessor(i);
              if (!Succ)
                continue;
              auto succSym =
                  InterproceduralEngine<D, Analysis>::getBlockSymbol(Succ);
              if (!outExpr)
                outExpr = Exp::hole(succSym);
              else
                outExpr = Exp::ndet(outExpr, Exp::hole(succSym));
            }
          }
        }
        if (!outExpr)
          outExpr = Exp::term(D::one());

        E currentPath = outExpr;
        for (auto It = BB.rbegin(); It != BB.rend(); ++It) {
          llvm::Instruction &I = *It;
          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (auto *Callee = CI->getCalledFunction()) {
              if (!Callee->isDeclaration()) {
                if (visited.insert(Callee).second)
                  worklist.push_back(Callee);
                currentPath = Exp::seq(
                    getCallReturnTransfer(analysis, *CI, *Callee, 0),
                    currentPath);
                currentPath =
                    Exp::call(InterproceduralEngine<D, Analysis>::getFuncSymbol(
                                  Callee),
                              currentPath);
                currentPath = Exp::seq(
                    getCallEntryTransfer(analysis, *CI, *Callee, 0), currentPath);
              } else {
                currentPath =
                    Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
              }
            } else {
              currentPath =
                  Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
            }
          }
          currentPath = analysis.getTransfer(I, currentPath);
        }

        eqns.emplace_back(bSym, currentPath);
      }

      if (Entry) {
        eqns.emplace_back(
            fSym,
            Exp::hole(InterproceduralEngine<D, Analysis>::getBlockSymbol(Entry)));
      } else {
        eqns.emplace_back(fSym, Exp::term(D::one()));
      }
    }

    auto rawRes = NewtonSolver<D>::solve(eqns, verbose);
    std::unordered_map<Symbol, Val> solvedMap;
    for (auto &p : rawRes.first)
      solvedMap[p.first] = p.second;

    Result res;
    for (const auto &entry : functionSymbols) {
      auto It = solvedMap.find(entry.first);
      if (It != solvedMap.end())
        res.summaries[entry.second] = It->second;
    }

    std::deque<llvm::Function *> worklist2;
    std::set<llvm::Function *> inWorklist2;
    std::unordered_map<std::string, Fact> funcExitFacts;
    std::unordered_map<std::string, size_t> funcUpdates;
    const long maxPropagationSteps = getMaxPropagationSteps(analysis, 0);
    long propagationSteps = 0;

    for (llvm::Function *Entry : entries) {
      std::string sym = InterproceduralEngine<D, Analysis>::getFuncSymbol(Entry);
      funcExitFacts[sym] = analysis.getExitValue(*Entry);
      worklist2.push_back(Entry);
      inWorklist2.insert(Entry);
    }

    while (!worklist2.empty()) {
      if (maxPropagationSteps >= 0 && propagationSteps++ >= maxPropagationSteps) {
        if (verbose)
          std::cerr << "[interproc-bwd] hit max propagation steps="
                    << maxPropagationSteps << "\n";
        break;
      }
      llvm::Function *F = worklist2.front();
      worklist2.pop_front();
      inWorklist2.erase(F);

      std::string fSym = InterproceduralEngine<D, Analysis>::getFuncSymbol(F);
      Fact exitFact = funcExitFacts[fSym];

      for (auto &BB : *F) {
        std::string bSym =
            InterproceduralEngine<D, Analysis>::getBlockSymbol(&BB);
        auto SolvedBlockIt = solvedMap.find(bSym);
        if (SolvedBlockIt == solvedMap.end())
          continue;

        res.blockEntryFacts[{&BB}] =
            analysis.applySummary(SolvedBlockIt->second, exitFact);

        Val blockEndToExit = D::one();
        auto *Term = BB.getTerminator();
        if (Term && Term->getNumSuccessors() > 0) {
          bool first = true;
          for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
            auto *Succ = Term->getSuccessor(i);
            if (!Succ)
              continue;
            auto succSym =
                InterproceduralEngine<D, Analysis>::getBlockSymbol(Succ);
            auto SuccIt = solvedMap.find(succSym);
            if (SuccIt == solvedMap.end())
              continue;
            if (first) {
              blockEndToExit = SuccIt->second;
              first = false;
            } else {
              blockEndToExit = D::combine(blockEndToExit, SuccIt->second);
            }
          }
        }

        E currentPath = Exp::term(D::one());
        for (auto It = BB.rbegin(); It != BB.rend(); ++It) {
          llvm::Instruction &I = *It;
          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (auto *Callee = CI->getCalledFunction()) {
              if (!Callee->isDeclaration()) {
                std::string calleeSym =
                    InterproceduralEngine<D, Analysis>::getFuncSymbol(Callee);
                Val currentPathVal = I0<D>::eval(false, solvedMap, currentPath);
                Val afterCallToExit = D::extend(currentPathVal, blockEndToExit);
                Val calleeExitToExit =
                    D::extend(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              afterCallToExit);
                Fact calleeExitFact =
                    analysis.applySummary(calleeExitToExit, exitFact);

                auto Existing = funcExitFacts.find(calleeSym);
                if (Existing == funcExitFacts.end()) {
                  funcExitFacts[calleeSym] = calleeExitFact;
                  if (inWorklist2.insert(Callee).second)
                    worklist2.push_back(Callee);
                } else {
                  Fact joined =
                      analysis.joinFacts(Existing->second, calleeExitFact);
                  if (!analysis.factsEqual(joined, Existing->second)) {
                    size_t updateCount = ++funcUpdates[calleeSym];
                    Fact widened = widenFacts(analysis, Existing->second, joined,
                                              updateCount, 0);
                    if (analysis.factsEqual(widened, Existing->second))
                      continue;
                    Existing->second = widened;
                    if (inWorklist2.insert(Callee).second)
                      worklist2.push_back(Callee);
                  }
                }

                currentPath = Exp::seq(
                    getCallReturnTransfer(analysis, *CI, *Callee, 0),
                    currentPath);
                currentPath = Exp::call(calleeSym, currentPath);
                currentPath = Exp::seq(
                    getCallEntryTransfer(analysis, *CI, *Callee, 0), currentPath);
              } else {
                currentPath =
                    Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
              }
            } else {
              currentPath =
                  Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
            }
          }
          currentPath = analysis.getTransfer(I, currentPath);
        }
      }
    }

    return res;
  }
};

} // namespace npa

#endif // NPA_BACKWARD_INTERPROCEDURAL_ENGINE_H
