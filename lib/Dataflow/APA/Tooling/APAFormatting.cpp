#include "Dataflow/APA/Tooling/APAFormatting.h"

#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <sstream>

using namespace llvm;

namespace elimination::tooling {

lotus::dataflow_tool::ValueIdMap buildModuleValueIdMap(Module &M) {
  lotus::dataflow_tool::ValueIdMap ValueToId;
  for (auto &G : M.globals()) {
    ValueToId[&G] = (G.hasName() ? G.getName() : "global").str();
  }

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    const std::string Prefix = F.getName().str();
    unsigned ArgIdx = 0;
    for (auto &Arg : F.args()) {
      ValueToId[&Arg] = Prefix + ".arg" + std::to_string(ArgIdx++);
    }

    unsigned InstIdx = 0;
    for (auto &BB : F) {
      for (auto &I : BB) {
        ValueToId[&I] = Prefix + ".i" + std::to_string(InstIdx++);
      }
    }
  }
  return ValueToId;
}

const char *toString(EliminationMethod M) {
  switch (M) {
  case EliminationMethod::StateElimination:
    return "state";
  case EliminationMethod::ADTSimple:
    return "adt-simple";
  case EliminationMethod::ADTDelayed:
    return "adt-delayed";
  }
  return "unknown";
}

const char *toString(SolveStatus S) {
  switch (S) {
  case SolveStatus::Ok:
    return "ok";
  case SolveStatus::FallbackToState:
    return "fallback-to-state";
  case SolveStatus::NonConvergentStar:
    return "non-convergent-star";
  case SolveStatus::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

const char *toString(FallbackReason R) {
  switch (R) {
  case FallbackReason::None:
    return "none";
  case FallbackReason::ADTRejected:
    return "adt-rejected";
  case FallbackReason::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

const char *toString(ADTRejectionReason R) {
  switch (R) {
  case ADTRejectionReason::None:
    return "none";
  case ADTRejectionReason::EmptyTopologicalOrder:
    return "empty-topological-order";
  case ADTRejectionReason::DisconnectedFromEntry:
    return "disconnected-from-entry";
  case ADTRejectionReason::NonBackEdgeCycle:
    return "non-back-edge-cycle";
  case ADTRejectionReason::EntryNotFirst:
    return "entry-not-first";
  case ADTRejectionReason::MissingTopologicalNode:
    return "missing-topological-node";
  case ADTRejectionReason::InvalidImmediateDominator:
    return "invalid-immediate-dominator";
  case ADTRejectionReason::ADTConstructionFailed:
    return "adt-construction-failed";
  case ADTRejectionReason::MissingADTLeaf:
    return "missing-adt-leaf";
  case ADTRejectionReason::EdgeClassificationFailed:
    return "edge-classification-failed";
  case ADTRejectionReason::ForwardEdgeMissesIntervalEntry:
    return "forward-edge-misses-interval-entry";
  case ADTRejectionReason::BackEdgeMissesIntervalEntry:
    return "back-edge-misses-interval-entry";
  }
  return "unknown";
}

std::string formatExpressionKey(const ExpressionKey &Key) {
  std::ostringstream ss;
  ss << "op" << Key.Opcode << "(";
  for (size_t i = 0; i < Key.Ops.size(); ++i) {
    if (i)
      ss << ",";
    ss << Key.Ops[i];
  }
  ss << ")";
  return ss.str();
}

std::string formatValueLatticeElement(const ConstantPropagationValue &Val) {
  std::ostringstream ss;
  if (Val.isUndef())
    ss << "undef";
  else if (Val.isUnknown())
    ss << "unknown";
  else if (Val.isOverdefined())
    ss << "overdefined";
  else if (Val.isNotConstant())
    ss << "notconst";
  else if (Val.isConstant()) {
    if (auto *CI = dyn_cast<ConstantInt>(Val.getConstant()))
      ss << "const" << CI->getZExtValue();
    else
      ss << "const";
  } else
    ss << "lattice";
  return ss.str();
}

std::string formatSignValue(SignValue Val) {
  if (Val.isBottom())
    return "bottom";
  std::string Result;
  auto Add = [&](bool Enabled, llvm::StringRef Name) {
    if (!Enabled)
      return;
    if (!Result.empty())
      Result += "|";
    Result += Name.str();
  };
  Add(Val.mayBeNegative(), "neg");
  Add(Val.mayBeZero(), "zero");
  Add(Val.mayBePositive(), "pos");
  return Result;
}

std::string serializeAffine(const AffineRelation &R) {
  if (R.bottom)
    return "bottom";
  std::string Buf;
  raw_string_ostream OS(Buf);
  bool FirstComp = true;
  for (const auto &Entry : R.components) {
    if (!FirstComp)
      OS << "|";
    FirstComp = false;
    OS << "w" << Entry.first << ":";
    std::vector<std::string> Rows;
    Rows.reserve(Entry.second.constraints.size());
    for (const auto &Row : Entry.second.constraints) {
      std::string RowStr;
      for (std::size_t i = 0; i < Row.size(); ++i) {
        if (i)
          RowStr += ",";
        RowStr += std::to_string(Row[i].getZExtValue());
      }
      Rows.push_back(std::move(RowStr));
    }
    std::sort(Rows.begin(), Rows.end());
    bool FirstRow = true;
    for (auto &Row : Rows) {
      if (!FirstRow)
        OS << ";";
      FirstRow = false;
      OS << Row;
    }
  }
  return OS.str();
}

std::string formatTransfer(const Instruction *I,
                           const lotus::dataflow_tool::ValueIdMap &ValueToId) {
  if (!I)
    return "null";
  auto It = ValueToId.find(I);
  return It != ValueToId.end() ? It->second : "inst";
}

std::string formatTransfer(const NonNullEdgeTransfer &Transfer,
                           const lotus::dataflow_tool::ValueIdMap &ValueToId) {
  return formatTransfer(Transfer.Src, ValueToId) + "->" +
         formatTransfer(Transfer.Dst, ValueToId);
}

std::string formatTransfer(const AffineEdgeTransfer &Transfer,
                           const lotus::dataflow_tool::ValueIdMap &ValueToId) {
  return formatTransfer(Transfer.inst, ValueToId) + "->" +
         formatTransfer(Transfer.succ, ValueToId);
}

}
