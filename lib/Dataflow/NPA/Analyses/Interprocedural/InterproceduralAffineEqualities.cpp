/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralAffineEqualities.h"

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"

#include <tuple>

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

bool AffineOp::operator<(const AffineOp &other) const {
  return std::tie(kind, dest, lhs, rhs, cond, constant, inputs) <
         std::tie(other.kind, other.dest, other.lhs, other.rhs, other.cond,
                  other.constant, other.inputs);
}

bool AffineOp::operator==(const AffineOp &other) const {
  return kind == other.kind && dest == other.dest && lhs == other.lhs &&
         rhs == other.rhs && cond == other.cond &&
         constant == other.constant && inputs == other.inputs;
}

namespace {

using D = AffineDomain;
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

AffineExpr topExpr() { return {}; }

AffineExpr constExpr(int64_t value) {
  AffineExpr out;
  out.top = false;
  out.constant = value;
  return out;
}

AffineExpr symbolicExpr(const llvm::Value *V) {
  AffineExpr out;
  out.top = false;
  out.terms[V] = 1;
  return out;
}

AffineExpr addExpr(AffineExpr lhs, const AffineExpr &rhs) {
  if (lhs.top || rhs.top)
    return topExpr();
  lhs.constant += rhs.constant;
  for (const auto &term : rhs.terms) {
    lhs.terms[term.first] += term.second;
    if (lhs.terms[term.first] == 0)
      lhs.terms.erase(term.first);
  }
  return lhs;
}

AffineExpr scaleExpr(AffineExpr expr, int64_t factor) {
  if (expr.top)
    return topExpr();
  expr.constant *= factor;
  for (auto It = expr.terms.begin(); It != expr.terms.end();) {
    It->second *= factor;
    if (It->second == 0)
      It = expr.terms.erase(It);
    else
      ++It;
  }
  return expr;
}

class AffineEqualitiesAnalysis {
public:
  using FactType = AffineState;

  FactType getEntryValue() const { return {true, {}}; }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I))
      return currentPath;
    if (I.getType()->isVoidTy())
      return currentPath;

    AffineOp op;
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

    AffineOp op;
    op.dest = &Call;
    op.kind = AffineOp::Kind::Phi;
    for (const auto &BB : Callee) {
      auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
      if (!Ret)
        continue;
      if (const llvm::Value *RetVal = Ret->getReturnValue())
        op.inputs.push_back(RetVal);
    }
    if (op.inputs.empty())
      op.kind = AffineOp::Kind::Forget;
    return D::singleton(op);
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::one();
    AffineOp op;
    op.kind = AffineOp::Kind::Forget;
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
      if (entry.second == It->second)
        out.values[entry.first] = entry.second;
    }
    return out;
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return lhs == rhs;
  }

private:
  FactType topFact(const FactType &fact) const {
    return {fact.reachable, {}};
  }

  D::value_type buildAssign(const llvm::Value *dest, const llvm::Value *src) const {
    AffineOp op;
    op.dest = dest;
    int64_t value = 0;
    if (getConstantIntValue(src, value)) {
      op.kind = AffineOp::Kind::AssignConst;
      op.constant = value;
    } else if (isTrackedScalar(src)) {
      op.kind = AffineOp::Kind::Copy;
      op.lhs = src;
    } else {
      op.kind = AffineOp::Kind::Forget;
    }
    return D::singleton(op);
  }

  bool buildTransfer(llvm::Instruction &I, AffineOp &op) const {
    op.dest = &I;
    if (!isTrackedScalar(&I)) {
      op.kind = AffineOp::Kind::Forget;
      return true;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      op.kind = AffineOp::Kind::Phi;
      for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i)
        op.inputs.push_back(Phi->getIncomingValue(i));
      return true;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      op.kind = AffineOp::Kind::Select;
      op.cond = Select->getCondition();
      op.lhs = Select->getTrueValue();
      op.rhs = Select->getFalseValue();
      return true;
    }

    if (llvm::isa<llvm::ICmpInst>(&I)) {
      op.kind = AffineOp::Kind::Forget;
      return true;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      int64_t value = 0;
      if (getConstantIntValue(Cast->getOperand(0), value)) {
        op.kind = AffineOp::Kind::AssignConst;
        op.constant = value;
      } else if (isTrackedScalar(Cast->getOperand(0))) {
        op.kind = AffineOp::Kind::Copy;
        op.lhs = Cast->getOperand(0);
      } else {
        op.kind = AffineOp::Kind::Forget;
      }
      return true;
    }

    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      llvm::Value *L = BinOp->getOperand(0);
      llvm::Value *R = BinOp->getOperand(1);
      int64_t C = 0;
      switch (BinOp->getOpcode()) {
      case llvm::Instruction::Add:
        op.kind = AffineOp::Kind::Add;
        op.lhs = L;
        op.rhs = R;
        return true;
      case llvm::Instruction::Sub:
        op.kind = AffineOp::Kind::Sub;
        op.lhs = L;
        op.rhs = R;
        return true;
      case llvm::Instruction::Mul:
        if (getConstantIntValue(L, C) && isTrackedScalar(R)) {
          op.kind = AffineOp::Kind::Scale;
          op.lhs = R;
          op.constant = C;
          return true;
        }
        if (getConstantIntValue(R, C) && isTrackedScalar(L)) {
          op.kind = AffineOp::Kind::Scale;
          op.lhs = L;
          op.constant = C;
          return true;
        }
        op.kind = AffineOp::Kind::Forget;
        return true;
      default:
        op.kind = AffineOp::Kind::Forget;
        return true;
      }
    }

    int64_t value = 0;
    if (getConstantIntValue(&I, value)) {
      op.kind = AffineOp::Kind::AssignConst;
      op.constant = value;
      return true;
    }

    op.kind = AffineOp::Kind::Forget;
    return true;
  }

  AffineExpr readExpr(const FactType &state, const llvm::Value *V) const {
    int64_t value = 0;
    if (getConstantIntValue(V, value))
      return constExpr(value);
    if (!isTrackedScalar(V))
      return topExpr();
    auto It = state.values.find(V);
    if (It != state.values.end())
      return It->second;
    if (llvm::isa<llvm::Argument>(V))
      return symbolicExpr(V);
    return topExpr();
  }

  void writeExpr(FactType &state, const llvm::Value *dest,
                 const AffineExpr &expr) const {
    if (!isTrackedScalar(dest))
      return;
    if (expr.top)
      state.values.erase(dest);
    else
      state.values[dest] = expr;
  }

  void applyOp(FactType &state, const AffineOp &op) const {
    switch (op.kind) {
    case AffineOp::Kind::AssignConst:
      writeExpr(state, op.dest, constExpr(op.constant));
      return;
    case AffineOp::Kind::Copy:
      writeExpr(state, op.dest, readExpr(state, op.lhs));
      return;
    case AffineOp::Kind::Add:
      writeExpr(state, op.dest,
                addExpr(readExpr(state, op.lhs), readExpr(state, op.rhs)));
      return;
    case AffineOp::Kind::Sub:
      writeExpr(state, op.dest,
                addExpr(readExpr(state, op.lhs),
                        scaleExpr(readExpr(state, op.rhs), -1)));
      return;
    case AffineOp::Kind::Scale:
      writeExpr(state, op.dest, scaleExpr(readExpr(state, op.lhs), op.constant));
      return;
    case AffineOp::Kind::Select: {
      int64_t cond = 0;
      if (getConstantIntValue(op.cond, cond)) {
        writeExpr(state, op.dest, readExpr(state, cond != 0 ? op.lhs : op.rhs));
      } else {
        AffineExpr lhs = readExpr(state, op.lhs);
        AffineExpr rhs = readExpr(state, op.rhs);
        writeExpr(state, op.dest, lhs == rhs ? lhs : topExpr());
      }
      return;
    }
    case AffineOp::Kind::Phi: {
      bool first = true;
      AffineExpr merged = topExpr();
      for (const llvm::Value *Input : op.inputs) {
        AffineExpr current = readExpr(state, Input);
        if (first) {
          merged = current;
          first = false;
        } else if (!(merged == current)) {
          merged = topExpr();
        }
      }
      writeExpr(state, op.dest, first ? topExpr() : merged);
      return;
    }
    case AffineOp::Kind::Forget:
      state.values.erase(op.dest);
      return;
    }
  }
};

} // namespace

InterproceduralAffineEqualities::Result
InterproceduralAffineEqualities::run(llvm::Module &M, bool verbose) {
  AffineEqualitiesAnalysis analysis;
  auto engineResult =
      InterproceduralEngine<AffineDomain, AffineEqualitiesAnalysis>::run(
          M, analysis, verbose);

  Result result;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  return result;
}

} // namespace npa
