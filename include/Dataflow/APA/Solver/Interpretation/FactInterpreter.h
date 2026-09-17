#pragma once

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Core/Timing.h"

#include <cassert>
#include <unordered_map>
#include <unordered_set>

namespace elimination::detail {

// Input-sensitive interpretation. A cache is valid only while the problem's
// transfer semantics (including external callee facts) remain unchanged.
template <typename ProblemT> class FactInterpreter final {
public:
  using Fact = typename ProblemT::fact_t;
  using Factory = PathExprFactory<typename ProblemT::transfer_t>;
  using Ref = typename Factory::Ref;

  FactInterpreter(const ProblemT &Problem, const EliminationOptions &Options,
                  SolveDiagnostics &Diagnostics, bool &Failed)
      : Problem(Problem), Options(Options), Diagnostics(Diagnostics),
        Failed(Failed) {}

  Fact eval(const Ref &Root, const Fact &Input) {
    assert(Root && "expression must not be null");
    if (Failed &&
        Options.NonConvergentStarPolicy == OnNonConvergentStar::Fail) {
      return Problem.bottom();
    }
    if (Options.MemoizeInterpretation) {
      auto Found = Memo.find(Root.get());
      if (Found != Memo.end()) {
        for (const auto &Entry : Found->second.Values) {
          if (Problem.equal(Entry.first, Input)) {
            return Entry.second;
          }
        }
      }
    }
    auto Result = evalFresh(Root, Input);
    if (Options.MemoizeInterpretation && !Failed && !Diagnostics.max_star_hit &&
        Entries < Options.InterpretationMemoBudget) {
      auto &Slot = Memo[Root.get()];
      Slot.Root = Root;
      Slot.Values.emplace_back(Input, Result);
      ++Entries;
    }
    return Result;
  }

  void resetMemo() {
    Memo.clear();
    Entries = 0;
  }

  // Immutable snapshot for this input. Root-only transformer clients bind their
  // compositional unit; state-based clients must know the corresponding input
  // before treating a cached Star as reusable. Retain operands to prevent
  // address reuse.
  std::function<bool(const void *)> starCacheSnapshot(const Fact &Input) const {
    struct Snapshot {
      std::unordered_set<const void *> Operands;
      std::vector<Ref> Roots;
    };
    auto Saved = std::make_shared<Snapshot>();
    for (const auto &Item : Memo) {
      if (Item.second.Root->K != Factory::Kind::Star)
        continue;
      for (const auto &Entry : Item.second.Values) {
        if (Problem.equal(Entry.first, Input)) {
          Saved->Operands.insert(Item.second.Root->L.get());
          Saved->Roots.push_back(Item.second.Root->L);
          break;
        }
      }
    }
    return [Saved](const void *Operand) {
      return Saved->Operands.count(Operand) != 0;
    };
  }

private:
  Fact evalFresh(const Ref &Root, const Fact &Input) {
    switch (Root->K) {
    case Factory::Kind::Zero:
      return Problem.bottom();
    case Factory::Kind::One:
      return Input;
    case Factory::Kind::Atom:
      return Problem.applyTransfer(*Root->Transfer, Input);
    case Factory::Kind::Union: {
      auto Left = eval(Root->L, Input);
      auto Right = eval(Root->R, Input);
      return Problem.join(Left, Right);
    }
    case Factory::Kind::Concat: {
      auto Mid = eval(Root->L, Input);
      return eval(Root->R, Mid);
    }
    case Factory::Kind::Star: {
      ScopedNestedNanoseconds Timer(Diagnostics.semantic_star_time_ns,
                                    StarDepth);
      auto Current = Input;
      const auto Limit = Options.MaxStarIterations
                             ? Options.MaxStarIterations
                             : Problem.maxStarIterations();
      for (std::size_t I = 0; I < Limit; ++I) {
        ++Diagnostics.star_iterations_total;
        auto Next = Problem.join(Input, eval(Root->L, Current));
        if (Problem.equal(Next, Current))
          return Current;
        Current = std::move(Next);
      }
      Diagnostics.max_star_hit = true;
      switch (Options.NonConvergentStarPolicy) {
      case OnNonConvergentStar::ReturnLast:
        return Current;
      case OnNonConvergentStar::ReturnIdentity:
        return Problem.bottom();
      case OnNonConvergentStar::Fail:
        Failed = true;
        return Problem.bottom();
      }
    }
    }
    Failed = true;
    return Problem.bottom();
  }
  struct Slot {
    Ref Root;
    std::vector<std::pair<Fact, Fact>> Values;
  };
  const ProblemT &Problem;
  const EliminationOptions &Options;
  SolveDiagnostics &Diagnostics;
  bool &Failed;
  std::size_t Entries = 0, StarDepth = 0;
  std::unordered_map<const typename Factory::Expr *, Slot> Memo;
};

} // namespace elimination::detail
