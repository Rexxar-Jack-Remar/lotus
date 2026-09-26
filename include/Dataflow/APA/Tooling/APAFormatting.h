#pragma once

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"
#include "Dataflow/APA/Domains/AffineTransfer.h"
#include "Dataflow/APA/Domains/ConstantPropagationDomain.h"
#include "Dataflow/APA/Domains/ExpressionKey.h"
#include "Dataflow/APA/Domains/NonNullDomain.h"
#include "Dataflow/APA/Domains/SignDomain.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <string>

namespace elimination::tooling {

lotus::dataflow_tool::ValueIdMap buildModuleValueIdMap(llvm::Module &M);

template <typename ContextT>
std::string contextToString(const ContextT &Ctx) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  Ctx.print(OS);
  return OS.str();
}

const char *toString(EliminationMethod M);
const char *toString(SolveStatus S);
const char *toString(FallbackReason R);
const char *toString(ADTRejectionReason R);

std::string formatExpressionKey(const ExpressionKey &Key);
std::string formatValueLatticeElement(const ConstantPropagationValue &Val);
std::string formatSignValue(SignValue Val);
std::string serializeAffine(const AffineRelation &R);

std::string formatTransfer(const llvm::Instruction *I,
                           const lotus::dataflow_tool::ValueIdMap &ValueToId);
std::string formatTransfer(const NonNullEdgeTransfer &Transfer,
                           const lotus::dataflow_tool::ValueIdMap &ValueToId);
std::string formatTransfer(const AffineEdgeTransfer &Transfer,
                           const lotus::dataflow_tool::ValueIdMap &ValueToId);

template <typename ExprRefT>
void formatPathExpr(llvm::raw_ostream &OS, const ExprRefT &Expr,
                    const lotus::dataflow_tool::ValueIdMap &ValueToId) {
  if (!Expr) {
    OS << "null";
    return;
  }

  using Kind = decltype(Expr->K);
  switch (Expr->K) {
  case Kind::Zero:
    OS << "zero";
    return;
  case Kind::One:
    OS << "one";
    return;
  case Kind::Atom:
    OS << "atom(";
    if (Expr->Transfer)
      OS << formatTransfer(*Expr->Transfer, ValueToId);
    else
      OS << "null";
    OS << ")";
    return;
  case Kind::Union:
    OS << "union(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case Kind::Concat:
    OS << "concat(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case Kind::Star:
    OS << "star(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ")";
    return;
  }
}

}

