#include "Verification/Frontend/BooleanProgram.h"

namespace lotus {
namespace verification {
namespace frontend {

BooleanExpr BooleanExpr::makeConstant(bool value) {
  BooleanExpr expr;
  expr.kind = ExprKind::Constant;
  expr.constant = value;
  return expr;
}

BooleanExpr BooleanExpr::makeVariable(std::string value, bool primed) {
  BooleanExpr expr;
  expr.kind = primed ? ExprKind::PrimedVariable : ExprKind::Variable;
  expr.name = std::move(value);
  return expr;
}

BooleanExpr BooleanExpr::makeNondet() {
  BooleanExpr expr;
  expr.kind = ExprKind::Nondet;
  return expr;
}

BooleanExpr BooleanExpr::makeChoose(BooleanExpr lhs, BooleanExpr rhs) {
  BooleanExpr expr;
  expr.kind = ExprKind::Choose;
  expr.operands.push_back(std::move(lhs));
  expr.operands.push_back(std::move(rhs));
  return expr;
}

BooleanExpr BooleanExpr::makeUnary(ExprKind kind, BooleanExpr operand) {
  BooleanExpr expr;
  expr.kind = kind;
  expr.operands.push_back(std::move(operand));
  return expr;
}

BooleanExpr BooleanExpr::makeBinary(ExprKind kind, BooleanExpr lhs,
                                    BooleanExpr rhs) {
  BooleanExpr expr;
  expr.kind = kind;
  expr.operands.push_back(std::move(lhs));
  expr.operands.push_back(std::move(rhs));
  return expr;
}

BooleanExpr BooleanExpr::makeTernary(BooleanExpr cond, BooleanExpr then_expr,
                                     BooleanExpr else_expr) {
  BooleanExpr expr;
  expr.kind = ExprKind::Ite;
  expr.operands.push_back(std::move(cond));
  expr.operands.push_back(std::move(then_expr));
  expr.operands.push_back(std::move(else_expr));
  return expr;
}

const Procedure *BooleanProgram::findProcedure(const std::string &name) const {
  for (const Procedure &procedure : procedures) {
    if (procedure.name == name)
      return &procedure;
  }
  return nullptr;
}

FrontendException::FrontendException(const FrontendError &error)
    : std::runtime_error(error.message), error_(error) {}

const LoweredNode *LoweringResult::findNode(const std::string &label) const {
  for (const LoweredNode &node : nodes) {
    if (node.label == label)
      return &node;
  }
  return nullptr;
}

const LoweredInstruction *LoweringResult::findInstruction(const std::string &id) const {
  for (const LoweredInstruction &instruction : instructions) {
    if (instruction.id == id)
      return &instruction;
  }
  return nullptr;
}

const LoweredEdge *LoweringResult::findEdge(const std::string &from,
                                            const std::string &to) const {
  for (const LoweredEdge &edge : edges) {
    if (edge.from == from && edge.to == to)
      return &edge;
  }
  return nullptr;
}

} // namespace frontend
} // namespace verification
} // namespace lotus
