/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralConstantPropagation.h"

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"

#include <algorithm>
#include <tuple>

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

bool ConstantPropagationOp::operator<(const ConstantPropagationOp &other) const {
  return std::tie(kind, dest, lhs, rhs, cond, opcode, constant, inputs) <
         std::tie(other.kind, other.dest, other.lhs, other.rhs, other.cond,
                  other.opcode, other.constant, other.inputs);
}

bool ConstantPropagationOp::operator==(const ConstantPropagationOp &other) const {
  return kind == other.kind && dest == other.dest && lhs == other.lhs &&
         rhs == other.rhs && cond == other.cond && opcode == other.opcode &&
         constant == other.constant && inputs == other.inputs;
}

namespace {

using D = ConstantPropagationDomain;
using Exp = Exp0<D>;
using E = E0<D>;

bool isTrackedScalar(const llvm::Value *V) {
  auto *Ty = V ? V->getType() : nullptr;
  return Ty && Ty->isIntegerTy() && Ty->getIntegerBitWidth() <= 64;
}

ConstantPropagationValue topValue() { return {}; }

ConstantPropagationValue constValue(int64_t value) {
  ConstantPropagationValue out;
  out.tag = ConstantPropagationTag::Const;
  out.constant = value;
  return out;
}

bool getConstantIntValue(const llvm::Value *V, int64_t &out) {
  auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(V);
  if (!CI || CI->getBitWidth() > 64)
    return false;
  out = CI->getSExtValue();
  return true;
}

class ConstantPropagationAnalysis {
public:
  using FactType = ConstantPropagationState;

  FactType getEntryValue() const { return {true, {}}; }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I))
      return currentPath;
    if (I.getType()->isVoidTy())
      return currentPath;

    ConstantPropagationOp op;
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

    ConstantPropagationOp op;
    op.dest = &Call;
    op.kind = ConstantPropagationOp::Kind::Phi;
    for (const auto &BB : Callee) {
      auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
      if (!Ret)
        continue;
      if (const llvm::Value *RetVal = Ret->getReturnValue())
        op.inputs.push_back(RetVal);
    }
    if (op.inputs.empty())
      op.kind = ConstantPropagationOp::Kind::Forget;
    return D::singleton(op);
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::one();
    ConstantPropagationOp op;
    op.kind = ConstantPropagationOp::Kind::Forget;
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
    ConstantPropagationOp op;
    op.dest = dest;
    int64_t value = 0;
    if (getConstantIntValue(src, value)) {
      op.kind = ConstantPropagationOp::Kind::AssignConst;
      op.constant = value;
    } else if (isTrackedScalar(src)) {
      op.kind = ConstantPropagationOp::Kind::Copy;
      op.lhs = src;
    } else {
      op.kind = ConstantPropagationOp::Kind::Forget;
    }
    return D::singleton(op);
  }

  bool buildTransfer(llvm::Instruction &I, ConstantPropagationOp &op) const {
    op.dest = &I;
    if (!isTrackedScalar(&I)) {
      op.kind = ConstantPropagationOp::Kind::Forget;
      return true;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Phi;
      for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i)
        op.inputs.push_back(Phi->getIncomingValue(i));
      return true;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Select;
      op.cond = Select->getCondition();
      op.lhs = Select->getTrueValue();
      op.rhs = Select->getFalseValue();
      return true;
    }

    if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Compare;
      op.lhs = Cmp->getOperand(0);
      op.rhs = Cmp->getOperand(1);
      op.opcode = static_cast<unsigned>(Cmp->getPredicate());
      return true;
    }

    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Binary;
      op.lhs = BinOp->getOperand(0);
      op.rhs = BinOp->getOperand(1);
      op.opcode = BinOp->getOpcode();
      return true;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      int64_t value = 0;
      if (getConstantIntValue(Cast->getOperand(0), value)) {
        op.kind = ConstantPropagationOp::Kind::AssignConst;
        op.constant = value;
      } else if (isTrackedScalar(Cast->getOperand(0))) {
        op.kind = ConstantPropagationOp::Kind::Copy;
        op.lhs = Cast->getOperand(0);
      } else {
        op.kind = ConstantPropagationOp::Kind::Forget;
      }
      return true;
    }

    if (auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(&I)) {
      if (isTrackedScalar(Freeze->getOperand(0))) {
        op.kind = ConstantPropagationOp::Kind::Copy;
        op.lhs = Freeze->getOperand(0);
      } else {
        op.kind = ConstantPropagationOp::Kind::Forget;
      }
      return true;
    }

    int64_t value = 0;
    if (getConstantIntValue(&I, value)) {
      op.kind = ConstantPropagationOp::Kind::AssignConst;
      op.constant = value;
      return true;
    }

    op.kind = ConstantPropagationOp::Kind::Forget;
    return true;
  }

  ConstantPropagationValue readValue(const FactType &state,
                                     const llvm::Value *V) const {
    int64_t value = 0;
    if (getConstantIntValue(V, value))
      return constValue(value);
    if (!isTrackedScalar(V))
      return topValue();
    auto It = state.values.find(V);
    if (It == state.values.end())
      return topValue();
    return It->second;
  }

  void writeValue(FactType &state, const llvm::Value *dest,
                  ConstantPropagationValue value) const {
    if (!isTrackedScalar(dest))
      return;
    if (value.tag == ConstantPropagationTag::Const)
      state.values[dest] = value;
    else
      state.values.erase(dest);
  }

  ConstantPropagationValue evalBinary(unsigned opcode,
                                      ConstantPropagationValue lhs,
                                      ConstantPropagationValue rhs) const {
    if (lhs.tag != ConstantPropagationTag::Const ||
        rhs.tag != ConstantPropagationTag::Const)
      return topValue();

    int64_t result = 0;
    switch (opcode) {
    case llvm::Instruction::Add:
      result = lhs.constant + rhs.constant;
      break;
    case llvm::Instruction::Sub:
      result = lhs.constant - rhs.constant;
      break;
    case llvm::Instruction::Mul:
      result = lhs.constant * rhs.constant;
      break;
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
      if (rhs.constant == 0)
        return topValue();
      result = lhs.constant / rhs.constant;
      break;
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem:
      if (rhs.constant == 0)
        return topValue();
      result = lhs.constant % rhs.constant;
      break;
    case llvm::Instruction::And:
      result = lhs.constant & rhs.constant;
      break;
    case llvm::Instruction::Or:
      result = lhs.constant | rhs.constant;
      break;
    case llvm::Instruction::Xor:
      result = lhs.constant ^ rhs.constant;
      break;
    case llvm::Instruction::Shl:
      result = lhs.constant << rhs.constant;
      break;
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
      result = lhs.constant >> rhs.constant;
      break;
    default:
      return topValue();
    }
    return constValue(result);
  }

  ConstantPropagationValue evalCompare(unsigned predicate,
                                       ConstantPropagationValue lhs,
                                       ConstantPropagationValue rhs) const {
    if (lhs.tag != ConstantPropagationTag::Const ||
        rhs.tag != ConstantPropagationTag::Const)
      return topValue();

    auto Pred = static_cast<llvm::CmpInst::Predicate>(predicate);
    bool result = false;
    switch (Pred) {
    case llvm::CmpInst::ICMP_EQ:
      result = lhs.constant == rhs.constant;
      break;
    case llvm::CmpInst::ICMP_NE:
      result = lhs.constant != rhs.constant;
      break;
    case llvm::CmpInst::ICMP_SGT:
    case llvm::CmpInst::ICMP_UGT:
      result = lhs.constant > rhs.constant;
      break;
    case llvm::CmpInst::ICMP_SGE:
    case llvm::CmpInst::ICMP_UGE:
      result = lhs.constant >= rhs.constant;
      break;
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_ULT:
      result = lhs.constant < rhs.constant;
      break;
    case llvm::CmpInst::ICMP_SLE:
    case llvm::CmpInst::ICMP_ULE:
      result = lhs.constant <= rhs.constant;
      break;
    default:
      return topValue();
    }
    return constValue(result ? 1 : 0);
  }

  ConstantPropagationValue joinValues(ConstantPropagationValue lhs,
                                      ConstantPropagationValue rhs) const {
    if (lhs.tag == ConstantPropagationTag::Const &&
        rhs.tag == ConstantPropagationTag::Const &&
        lhs.constant == rhs.constant)
      return lhs;
    return topValue();
  }

  void applyOp(FactType &state, const ConstantPropagationOp &op) const {
    switch (op.kind) {
    case ConstantPropagationOp::Kind::AssignConst:
      writeValue(state, op.dest, constValue(op.constant));
      return;
    case ConstantPropagationOp::Kind::Copy:
      writeValue(state, op.dest, readValue(state, op.lhs));
      return;
    case ConstantPropagationOp::Kind::Binary:
      writeValue(state, op.dest,
                 evalBinary(op.opcode, readValue(state, op.lhs),
                            readValue(state, op.rhs)));
      return;
    case ConstantPropagationOp::Kind::Compare:
      writeValue(state, op.dest,
                 evalCompare(op.opcode, readValue(state, op.lhs),
                             readValue(state, op.rhs)));
      return;
    case ConstantPropagationOp::Kind::Phi: {
      ConstantPropagationValue joined = topValue();
      bool first = true;
      for (const llvm::Value *Input : op.inputs) {
        auto value = readValue(state, Input);
        if (first) {
          joined = value;
          first = false;
        } else {
          joined = joinValues(joined, value);
        }
      }
      writeValue(state, op.dest, first ? topValue() : joined);
      return;
    }
    case ConstantPropagationOp::Kind::Select: {
      ConstantPropagationValue cond = readValue(state, op.cond);
      if (cond.tag == ConstantPropagationTag::Const) {
        writeValue(state, op.dest,
                   readValue(state, cond.constant != 0 ? op.lhs : op.rhs));
      } else {
        writeValue(state, op.dest,
                   joinValues(readValue(state, op.lhs), readValue(state, op.rhs)));
      }
      return;
    }
    case ConstantPropagationOp::Kind::Forget:
      state.values.erase(op.dest);
      return;
    }
  }
};

} // namespace

InterproceduralConstantPropagation::Result
InterproceduralConstantPropagation::run(llvm::Module &M, bool verbose) {
  ConstantPropagationAnalysis analysis;
  auto engineResult =
      InterproceduralEngine<ConstantPropagationDomain,
                            ConstantPropagationAnalysis>::run(M, analysis,
                                                              verbose);

  Result result;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  return result;
}

} // namespace npa
