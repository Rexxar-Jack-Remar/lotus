#ifndef NPA_INTERPROCEDURAL_ENGINE_H
#define NPA_INTERPROCEDURAL_ENGINE_H

#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/NPA/NPA.h"

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

struct FunctionKey {
  const llvm::Function *function = nullptr;

  bool operator<(const FunctionKey &other) const {
    return function < other.function;
  }
};

struct BlockKey {
  const llvm::BasicBlock *block = nullptr;

  bool operator<(const BlockKey &other) const { return block < other.block; }
};

template <class D, class Analysis> class InterproceduralEngine {
public:
  using Exp = Exp0<D>;
  using E = E0<D>;
  using Val = typename D::value_type;
  using Fact = typename Analysis::FactType;

  struct Result {
    std::map<FunctionKey, Val> summaries;
    std::map<BlockKey, Fact> blockEntryFacts;
  };

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
          auto *Callee = CB ? CB->getCalledFunction() : nullptr;
          if (Callee && !Callee->isDeclaration())
            called.insert(Callee);
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

  static Result run(llvm::Module &M, Analysis &analysis, bool verbose = false) {
    ::dataflow::controlflow::LLVMIntraCFG CFG;
    std::vector<std::pair<Symbol, E>> eqns;
    std::deque<llvm::Function *> worklist;
    std::set<llvm::Function *> visited;
    std::unordered_map<std::string, FunctionKey> functionSymbols;
    std::unordered_map<std::string, BlockKey> blockSymbols;

    std::vector<llvm::Function *> entries = getEntryFunctions(M);
    for (llvm::Function *Entry : entries) {
      worklist.push_back(Entry);
      visited.insert(Entry);
    }

    while (!worklist.empty()) {
      llvm::Function *F = worklist.front();
      worklist.pop_front();

      std::string fSym = getFuncSymbol(F);
      functionSymbols[fSym] = {F};
      E exitExpr = nullptr;

      for (auto &BB : *F) {
        std::string bSym = getBlockSymbol(&BB);
        blockSymbols[bSym] = {&BB};

        E inExpr = nullptr;
        if (&BB == &F->getEntryBlock()) {
          inExpr = Exp::term(D::one());
        } else {
          bool hasPreds = false;
          auto *First = BB.empty() ? nullptr : &BB.front();
          for (auto *PredInst : CFG.getPredsOf(
                   First, ::dataflow::controlflow::FlowDirection::Forward)) {
            auto *Pred = PredInst ? PredInst->getParent() : nullptr;
            if (!Pred)
              continue;
            hasPreds = true;
            auto pHole = Exp::hole(getBlockSymbol(Pred));
            if (!inExpr)
              inExpr = pHole;
            else
              inExpr = Exp::ndet(inExpr, pHole);
          }
          if (!hasPreds)
            inExpr = Exp::term(D::zero());
        }

        E currentPath = inExpr;
        for (auto &I : BB) {
          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (auto *Callee = CI->getCalledFunction()) {
              if (!Callee->isDeclaration()) {
                if (visited.insert(Callee).second)
                  worklist.push_back(Callee);
                currentPath =
                    Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                             currentPath);
                currentPath = Exp::call(getFuncSymbol(Callee), currentPath);
                currentPath =
                    Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                             currentPath);
              } else {
                currentPath = Exp::seq(
                    getCallToReturnTransfer(analysis, *CI, 0), currentPath);
              }
            } else {
              currentPath = Exp::seq(getCallToReturnTransfer(analysis, *CI, 0),
                                     currentPath);
            }
          }
          currentPath = analysis.getTransfer(I, currentPath);
        }

        eqns.emplace_back(bSym, currentPath);

        auto *Term = BB.getTerminator();
        if (Term == nullptr ||
            CFG.getSuccsOf(Term,
                           ::dataflow::controlflow::FlowDirection::Forward)
                .empty()) {
          if (!exitExpr)
            exitExpr = Exp::hole(bSym);
          else
            exitExpr = Exp::ndet(exitExpr, Exp::hole(bSym));
        }
      }

      if (!exitExpr)
        exitExpr = Exp::term(D::zero());
      eqns.emplace_back(fSym, exitExpr);
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
    std::unordered_map<std::string, Fact> funcInput;
    std::unordered_map<std::string, size_t> funcUpdates;
    const long maxPropagationSteps = getMaxPropagationSteps(analysis, 0);
    long propagationSteps = 0;

    for (llvm::Function *Entry : entries) {
      std::string sym = getFuncSymbol(Entry);
      funcInput[sym] = analysis.getEntryValue();
      worklist2.push_back(Entry);
      inWorklist2.insert(Entry);
    }

    while (!worklist2.empty()) {
      if (maxPropagationSteps >= 0 && propagationSteps++ >= maxPropagationSteps) {
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
        std::string bSym = getBlockSymbol(&BB);
        if (!solvedMap.count(bSym))
          continue;

        Val entryToBlockStart = D::zero();
        if (&BB == &F->getEntryBlock()) {
          entryToBlockStart = D::one();
        } else {
          bool first = true;
          auto *First = BB.empty() ? nullptr : &BB.front();
          for (auto *PredInst : CFG.getPredsOf(
                   First, ::dataflow::controlflow::FlowDirection::Forward)) {
            auto *Pred = PredInst ? PredInst->getParent() : nullptr;
            if (!Pred)
              continue;
            std::string pSym = getBlockSymbol(Pred);
            if (solvedMap.count(pSym)) {
              if (first) {
                entryToBlockStart = solvedMap[pSym];
                first = false;
              } else {
                entryToBlockStart =
                    D::combine(entryToBlockStart, solvedMap[pSym]);
              }
            }
          }
        }

        auto blockEntryFact =
            analysis.applySummary(entryToBlockStart, inputVal);
        res.blockEntryFacts[{&BB}] = blockEntryFact;

        E currentPath = Exp::term(D::one());

        for (auto &I : BB) {
          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (auto *Callee = CI->getCalledFunction()) {
              if (!Callee->isDeclaration()) {
                std::string calleeFSym = getFuncSymbol(Callee);

                Val currentPathVal = I0<D>::eval(false, solvedMap, currentPath);
                Val callEntry =
                    D::extend(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                              currentPathVal);
                Val totalToCall = D::extend(callEntry, entryToBlockStart);

                auto factAtCall = analysis.applySummary(totalToCall, inputVal);

                if (!funcInput.count(calleeFSym)) {
                  funcInput[calleeFSym] = factAtCall;
                  if (inWorklist2.insert(Callee).second)
                    worklist2.push_back(Callee);
                } else {
                  auto oldVal = funcInput[calleeFSym];
                  auto newVal = analysis.joinFacts(oldVal, factAtCall);
                  if (!analysis.factsEqual(oldVal, newVal)) {
                    size_t updateCount = ++funcUpdates[calleeFSym];
                    Fact widened =
                        widenFacts(analysis, oldVal, newVal, updateCount, 0);
                    if (analysis.factsEqual(oldVal, widened))
                      continue;
                    funcInput[calleeFSym] = widened;
                    if (inWorklist2.insert(Callee).second)
                      worklist2.push_back(Callee);
                  }
                }
              }
            }
          }

          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (auto *Callee = CI->getCalledFunction()) {
              if (!Callee->isDeclaration()) {
                currentPath =
                    Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                             currentPath);
                currentPath = Exp::call(getFuncSymbol(Callee), currentPath);
                currentPath =
                    Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                             currentPath);
              } else {
                currentPath = Exp::seq(
                    getCallToReturnTransfer(analysis, *CI, 0), currentPath);
              }
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

#endif
