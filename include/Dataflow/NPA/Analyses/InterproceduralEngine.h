#ifndef NPA_INTERPROCEDURAL_ENGINE_H
#define NPA_INTERPROCEDURAL_ENGINE_H

#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/NPA/NPA.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace npa {

using CallSiteID = const llvm::Instruction *;
using CallString = std::vector<CallSiteID>;

template <class D, class Analysis, int K = 0> class InterproceduralEngine {
public:
  using Exp = Exp0<D>;
  using E = E0<D>;
  using Val = typename D::value_type;
  using Fact = typename Analysis::FactType;

  struct Result {
    std::unordered_map<std::string, Val> summaries;
    std::unordered_map<std::string, Fact> blockEntryFacts;
  };

  static void appendCallString(std::string &s, const CallString &cs) {
    for (auto *site : cs) {
      s.append(reinterpret_cast<const char *>(&site), sizeof(site));
    }
  }

  static std::string getBlockSymbol(const llvm::BasicBlock *BB,
                                    const CallString &cs) {
    std::string s;
    s.reserve(1 + sizeof(BB) + cs.size() * sizeof(CallSiteID));
    s.push_back('B');
    s.append(reinterpret_cast<const char *>(&BB), sizeof(BB));
    appendCallString(s, cs);
    return s;
  }

  static std::string getFuncSymbol(const llvm::Function *F,
                                   const CallString &cs) {
    std::string s;
    s.reserve(1 + sizeof(F) + cs.size() * sizeof(CallSiteID));
    s.push_back('F');
    s.append(reinterpret_cast<const char *>(&F), sizeof(F));
    appendCallString(s, cs);
    return s;
  }

  static CallString pushContext(const CallString &cs,
                                const llvm::Instruction *site) {
    CallString next = cs;
    next.push_back(site);
    if (next.size() > K)
      next.erase(next.begin());
    return next;
  }

private:
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
    ::dataflow::controlflow::LLVMIntraCFG CFG;
    std::vector<std::pair<Symbol, E>> eqns;
    std::deque<std::pair<llvm::Function *, CallString>> worklist;
    std::set<std::pair<llvm::Function *, CallString>> visited;

    llvm::Function *Main = M.getFunction("main");
    if (Main) {
      worklist.push_back({Main, {}});
      visited.insert({Main, {}});
    } else {
      for (auto &F : M) {
        if (!F.isDeclaration()) {
          worklist.push_back({&F, {}});
          visited.insert({&F, {}});
        }
      }
    }

    while (!worklist.empty()) {
      auto item = worklist.front();
      worklist.pop_front();
      llvm::Function *F = item.first;
      CallString cs = item.second;

      std::string fSym = getFuncSymbol(F, cs);
      E exitExpr = nullptr;

      for (auto &BB : *F) {
        std::string bSym = getBlockSymbol(&BB, cs);

        E inExpr = nullptr;
        if (&BB == &F->getEntryBlock()) {
          inExpr = Exp::term(D::one());
        } else {
          bool hasPreds = false;
          auto *First = BB.empty() ? nullptr : &BB.front();
          for (auto *PredInst : CFG.getPredsOf(
                   First, ::dataflow::controlflow::FlowDirection::Forward)) {
            auto *Pred = PredInst ? PredInst->getParent() : nullptr;
            if (Pred == nullptr)
              continue;
            hasPreds = true;
            std::string predSym = getBlockSymbol(Pred, cs);
            auto pHole = Exp::hole(predSym);
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
                CallString calleeCS = pushContext(cs, CI);
                if (visited.find({Callee, calleeCS}) == visited.end()) {
                  visited.insert({Callee, calleeCS});
                  worklist.push_back({Callee, calleeCS});
                }
                currentPath =
                    Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                             currentPath);
                currentPath =
                    Exp::call(getFuncSymbol(Callee, calleeCS), currentPath);
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
    res.summaries = solvedMap;

    std::deque<std::pair<llvm::Function *, CallString>> worklist2;
    std::set<std::pair<llvm::Function *, CallString>> inWorklist2;
    std::unordered_map<std::string, Fact> funcInput;

    if (Main) {
      std::string sym = getFuncSymbol(Main, {});
      funcInput[sym] = analysis.getEntryValue();
      worklist2.push_back({Main, {}});
      inWorklist2.insert({Main, {}});
    } else {
      for (auto &F : M) {
        if (!F.isDeclaration()) {
          std::string sym = getFuncSymbol(&F, {});
          funcInput[sym] = analysis.getEntryValue();
          worklist2.push_back({&F, {}});
          inWorklist2.insert({&F, {}});
        }
      }
    }

    while (!worklist2.empty()) {
      auto item = worklist2.front();
      worklist2.pop_front();
      llvm::Function *F = item.first;
      CallString cs = item.second;
      inWorklist2.erase(item);

      std::string fSym = getFuncSymbol(F, cs);
      Fact inputVal = funcInput[fSym];

      for (auto &BB : *F) {
        std::string bSym = getBlockSymbol(&BB, cs);
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
            if (Pred == nullptr)
              continue;
            std::string pSym = getBlockSymbol(Pred, cs);
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
        res.blockEntryFacts[bSym] = blockEntryFact;

        E currentPath = Exp::term(D::one());

        for (auto &I : BB) {
          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (auto *Callee = CI->getCalledFunction()) {
              if (!Callee->isDeclaration()) {
                CallString calleeCS = pushContext(cs, CI);
                std::string calleeFSym = getFuncSymbol(Callee, calleeCS);

                Val currentPathVal = I0<D>::eval(false, solvedMap, currentPath);
                Val callEntry =
                    D::extend(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                              currentPathVal);
                Val totalToCall = D::extend(callEntry, entryToBlockStart);

                auto factAtCall = analysis.applySummary(totalToCall, inputVal);

                if (!funcInput.count(calleeFSym)) {
                  funcInput[calleeFSym] = factAtCall;
                  if (inWorklist2.find({Callee, calleeCS}) ==
                      inWorklist2.end()) {
                    worklist2.push_back({Callee, calleeCS});
                    inWorklist2.insert({Callee, calleeCS});
                  }
                } else {
                  auto oldVal = funcInput[calleeFSym];
                  auto newVal = analysis.joinFacts(oldVal, factAtCall);
                  if (!analysis.factsEqual(oldVal, newVal)) {
                    funcInput[calleeFSym] = newVal;
                    if (inWorklist2.find({Callee, calleeCS}) ==
                        inWorklist2.end()) {
                      worklist2.push_back({Callee, calleeCS});
                      inWorklist2.insert({Callee, calleeCS});
                    }
                  }
                }
              }
            }
          }

          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (auto *Callee = CI->getCalledFunction()) {
              if (!Callee->isDeclaration()) {
                CallString calleeCS = pushContext(cs, CI);
                currentPath =
                    Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                             currentPath);
                currentPath =
                    Exp::call(getFuncSymbol(Callee, calleeCS), currentPath);
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
