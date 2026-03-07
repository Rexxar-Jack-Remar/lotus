/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralIntervalAnalysis.h"

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"

#include <algorithm>
#include <array>
#include <tuple>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

bool IntervalOp::operator<(const IntervalOp &other) const {
  return std::tie(kind, dest, lhs, rhs, cond, opcode, constant, inputs) <
         std::tie(other.kind, other.dest, other.lhs, other.rhs, other.cond,
                  other.opcode, other.constant, other.inputs);
}

bool IntervalOp::operator==(const IntervalOp &other) const {
  return kind == other.kind && dest == other.dest && lhs == other.lhs &&
         rhs == other.rhs && cond == other.cond && opcode == other.opcode &&
         constant == other.constant && inputs == other.inputs;
}

namespace {

using D = IntervalDomain;
using Exp = Exp0<D>;
using E = E0<D>;

bool isTrackedScalar(const llvm::Value *V) {
  auto *Ty = V ? V->getType() : nullptr;
  return Ty && Ty->isIntegerTy() && Ty->getIntegerBitWidth() <= 64;
}

bool getConstantIntValue(const llvm::Value *V, int64_t &out) {
  auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(V);
  if (!CI || CI->getBitWidth() > 64)
    return false;
  out = CI->getSExtValue();
  return true;
}

Interval topInterval() { return Interval::top(); }

Interval joinIntervals(const Interval &lhs, const Interval &rhs) {
  if (lhs.bottom)
    return rhs;
  if (rhs.bottom)
    return lhs;
  if (!lhs.hasLower || !lhs.hasUpper || !rhs.hasLower || !rhs.hasUpper)
    return Interval::top();
  Interval out;
  out.hasLower = true;
  out.hasUpper = true;
  out.lower = std::min(lhs.lower, rhs.lower);
  out.upper = std::max(lhs.upper, rhs.upper);
  return out;
}

bool containsZero(const Interval &I) {
  if (I.bottom)
    return false;
  if (!I.hasLower || !I.hasUpper)
    return true;
  return I.lower <= 0 && I.upper >= 0;
}

class IntervalAnalysis {
public:
  using FactType = IntervalState;
  static constexpr long kMaxPropagationSteps = 256;

  FactType getEntryValue() const { return {true, {}}; }

  long getMaxPropagationSteps() const { return kMaxPropagationSteps; }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I))
      return currentPath;
    if (I.getType()->isVoidTy())
      return currentPath;

    IntervalOp op;
    if (!buildTransfer(I, op))
      return currentPath;
    return Exp::seq(D::singleton(op), currentPath);
  }

  D::value_type getCallEntryTransfer(const llvm::CallBase &Call,
                                     const llvm::Function &Callee) {
    D::value_type transfer = D::one();
    const auto *ParamIt = Callee.arg_begin();
    for (unsigned i = 0; i < Call.arg_size() && ParamIt != Callee.arg_end();
         ++i, ++ParamIt) {
      if (!isTrackedScalar(&*ParamIt))
        continue;
      transfer = D::extend(buildAssign(&*ParamIt, Call.getArgOperand(i)), transfer);
    }
    return transfer;
  }

  D::value_type getCallReturnTransfer(const llvm::CallBase &Call,
                                      const llvm::Function &Callee) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::one();

    IntervalOp op;
    op.dest = &Call;
    op.kind = IntervalOp::Kind::Phi;
    for (const auto &BB : Callee) {
      auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
      if (!Ret)
        continue;
      if (const llvm::Value *RetVal = Ret->getReturnValue())
        op.inputs.push_back(RetVal);
    }
    if (op.inputs.empty())
      op.kind = IntervalOp::Kind::Forget;
    return D::singleton(op);
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::one();
    IntervalOp op;
    op.kind = IntervalOp::Kind::Forget;
    op.dest = &Call;
    return D::singleton(op);
  }

  FactType applySummary(const D::value_type &summary, const FactType &fact) {
    if (!fact.reachable || summary.paths.empty())
      return summary.overflow ? topFact(fact) : FactType{false, {}};
    if (summary.overflow)
      return topFact(fact);

    bool first = true;
    FactType joined;
    for (const auto &path : summary.paths) {
      FactType current = fact;
      current.reachable = true;
      for (const auto &op : path)
        applyOp(current, op);
      if (first) {
        joined = std::move(current);
        first = false;
      } else {
        joined = joinFacts(joined, current);
      }
    }
    return first ? FactType{false, {}} : joined;
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    if (!lhs.reachable)
      return rhs;
    if (!rhs.reachable)
      return lhs;

    FactType out;
    out.reachable = true;
    for (const auto &entry : lhs.values) {
      auto It = rhs.values.find(entry.first);
      if (It == rhs.values.end())
        continue;
      Interval joined = joinIntervals(entry.second, It->second);
      if (!(joined == Interval::top()))
        out.values[entry.first] = joined;
    }
    return out;
  }

  FactType widenFacts(const FactType &oldFact, const FactType &newFact,
                      size_t updates) const {
    if (updates < 2 || !oldFact.reachable)
      return newFact;
    if (!newFact.reachable)
      return oldFact;

    FactType widened;
    widened.reachable = true;
    for (const auto &entry : newFact.values) {
      const llvm::Value *V = entry.first;
      const Interval &next = entry.second;
      auto OldIt = oldFact.values.find(V);
      if (OldIt == oldFact.values.end()) {
        widened.values[V] = next;
        continue;
      }
      widened.values[V] = widenInterval(OldIt->second, next);
    }
    return widened;
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return lhs == rhs;
  }

private:
  FactType topFact(const FactType &fact) const {
    return {fact.reachable, {}};
  }

  static Interval widenInterval(const Interval &oldI, const Interval &newI) {
    if (oldI.bottom)
      return newI;
    if (newI.bottom)
      return oldI;
    if (!oldI.hasLower || !oldI.hasUpper)
      return oldI;
    if (!newI.hasLower || !newI.hasUpper)
      return Interval::top();

    Interval out = newI;
    if (newI.lower < oldI.lower)
      out.hasLower = false;
    else {
      out.hasLower = true;
      out.lower = newI.lower;
    }
    if (newI.upper > oldI.upper)
      out.hasUpper = false;
    else {
      out.hasUpper = true;
      out.upper = newI.upper;
    }
    return out;
  }

  D::value_type buildAssign(const llvm::Value *dest, const llvm::Value *src) const {
    IntervalOp op;
    op.dest = dest;
    int64_t value = 0;
    if (getConstantIntValue(src, value)) {
      op.kind = IntervalOp::Kind::AssignConst;
      op.constant = value;
    } else if (isTrackedScalar(src)) {
      op.kind = IntervalOp::Kind::Copy;
      op.lhs = src;
    } else {
      op.kind = IntervalOp::Kind::Forget;
    }
    return D::singleton(op);
  }

  bool buildTransfer(llvm::Instruction &I, IntervalOp &op) const {
    op.dest = &I;
    if (!isTrackedScalar(&I)) {
      op.kind = IntervalOp::Kind::Forget;
      return true;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      op.kind = IntervalOp::Kind::Phi;
      for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i)
        op.inputs.push_back(Phi->getIncomingValue(i));
      return true;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      op.kind = IntervalOp::Kind::Select;
      op.cond = Select->getCondition();
      op.lhs = Select->getTrueValue();
      op.rhs = Select->getFalseValue();
      return true;
    }

    if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
      op.kind = IntervalOp::Kind::Compare;
      op.lhs = Cmp->getOperand(0);
      op.rhs = Cmp->getOperand(1);
      op.opcode = static_cast<unsigned>(Cmp->getPredicate());
      return true;
    }

    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      op.kind = IntervalOp::Kind::Binary;
      op.lhs = BinOp->getOperand(0);
      op.rhs = BinOp->getOperand(1);
      op.opcode = BinOp->getOpcode();
      return true;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      int64_t value = 0;
      if (getConstantIntValue(Cast->getOperand(0), value)) {
        op.kind = IntervalOp::Kind::AssignConst;
        op.constant = value;
      } else if (isTrackedScalar(Cast->getOperand(0))) {
        op.kind = IntervalOp::Kind::Copy;
        op.lhs = Cast->getOperand(0);
      } else {
        op.kind = IntervalOp::Kind::Forget;
      }
      return true;
    }

    op.kind = IntervalOp::Kind::Forget;
    return true;
  }

  Interval readValue(const FactType &state, const llvm::Value *V) const {
    int64_t value = 0;
    if (getConstantIntValue(V, value))
      return Interval::point(value);
    if (!isTrackedScalar(V))
      return topInterval();
    auto It = state.values.find(V);
    if (It == state.values.end())
      return topInterval();
    return It->second;
  }

  void writeValue(FactType &state, const llvm::Value *dest,
                  const Interval &value) const {
    if (!isTrackedScalar(dest))
      return;
    if (value == Interval::top())
      state.values.erase(dest);
    else
      state.values[dest] = value;
  }

  Interval evalBinary(unsigned opcode, Interval lhs, Interval rhs) const {
    if (!lhs.hasLower || !lhs.hasUpper || !rhs.hasLower || !rhs.hasUpper)
      return topInterval();

    Interval out;
    out.hasLower = true;
    out.hasUpper = true;
    switch (opcode) {
    case llvm::Instruction::Add:
      out.lower = lhs.lower + rhs.lower;
      out.upper = lhs.upper + rhs.upper;
      return out;
    case llvm::Instruction::Sub:
      out.lower = lhs.lower - rhs.upper;
      out.upper = lhs.upper - rhs.lower;
      return out;
    case llvm::Instruction::Mul: {
      std::array<int64_t, 4> products = {
          lhs.lower * rhs.lower, lhs.lower * rhs.upper, lhs.upper * rhs.lower,
          lhs.upper * rhs.upper};
      auto bounds = std::minmax_element(products.begin(), products.end());
      out.lower = *bounds.first;
      out.upper = *bounds.second;
      return out;
    }
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
      if (containsZero(rhs))
        return topInterval();
      if (rhs.lower == 0 || rhs.upper == 0)
        return topInterval();
      out.lower = std::min(lhs.lower / rhs.lower, lhs.upper / rhs.upper);
      out.upper = std::max(lhs.lower / rhs.lower, lhs.upper / rhs.upper);
      return out;
    default:
      return topInterval();
    }
  }

  Interval evalCompare(unsigned predicate, Interval lhs, Interval rhs) const {
    int64_t l = 0;
    int64_t r = 0;
    if (lhs.hasLower && lhs.hasUpper && lhs.lower == lhs.upper &&
        rhs.hasLower && rhs.hasUpper && rhs.lower == rhs.upper) {
      l = lhs.lower;
      r = rhs.lower;
      bool result = false;
      switch (static_cast<llvm::CmpInst::Predicate>(predicate)) {
      case llvm::CmpInst::ICMP_EQ:
        result = l == r;
        break;
      case llvm::CmpInst::ICMP_NE:
        result = l != r;
        break;
      case llvm::CmpInst::ICMP_SGT:
      case llvm::CmpInst::ICMP_UGT:
        result = l > r;
        break;
      case llvm::CmpInst::ICMP_SGE:
      case llvm::CmpInst::ICMP_UGE:
        result = l >= r;
        break;
      case llvm::CmpInst::ICMP_SLT:
      case llvm::CmpInst::ICMP_ULT:
        result = l < r;
        break;
      case llvm::CmpInst::ICMP_SLE:
      case llvm::CmpInst::ICMP_ULE:
        result = l <= r;
        break;
      default:
        return topInterval();
      }
      return Interval::point(result ? 1 : 0);
    }
    Interval out;
    out.hasLower = true;
    out.hasUpper = true;
    out.lower = 0;
    out.upper = 1;
    return out;
  }

  void applyOp(FactType &state, const IntervalOp &op) const {
    switch (op.kind) {
    case IntervalOp::Kind::AssignConst:
      writeValue(state, op.dest, Interval::point(op.constant));
      return;
    case IntervalOp::Kind::Copy:
      writeValue(state, op.dest, readValue(state, op.lhs));
      return;
    case IntervalOp::Kind::Binary:
      writeValue(state, op.dest,
                 evalBinary(op.opcode, readValue(state, op.lhs),
                            readValue(state, op.rhs)));
      return;
    case IntervalOp::Kind::Compare:
      writeValue(state, op.dest,
                 evalCompare(op.opcode, readValue(state, op.lhs),
                             readValue(state, op.rhs)));
      return;
    case IntervalOp::Kind::Select: {
      Interval cond = readValue(state, op.cond);
      if (cond.hasLower && cond.hasUpper && cond.lower == cond.upper) {
        writeValue(state, op.dest,
                   readValue(state, cond.lower != 0 ? op.lhs : op.rhs));
      } else {
        writeValue(state, op.dest,
                   joinIntervals(readValue(state, op.lhs),
                                 readValue(state, op.rhs)));
      }
      return;
    }
    case IntervalOp::Kind::Phi: {
      bool first = true;
      Interval joined = topInterval();
      for (const llvm::Value *Input : op.inputs) {
        Interval current = readValue(state, Input);
        if (first) {
          joined = current;
          first = false;
        } else {
          joined = joinIntervals(joined, current);
        }
      }
      writeValue(state, op.dest, first ? topInterval() : joined);
      return;
    }
    case IntervalOp::Kind::Forget:
      state.values.erase(op.dest);
      return;
    }
  }
};

} // namespace

InterproceduralIntervalAnalysis::Result
InterproceduralIntervalAnalysis::run(llvm::Module &M, bool verbose) {
  IntervalAnalysis analysis;
  auto engineResult =
      InterproceduralEngine<IntervalDomain, IntervalAnalysis>::run(M, analysis,
                                                                   verbose);

  Result result;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  return result;
}

} // namespace npa
