#pragma once

#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus {
namespace verification {
namespace frontend {

enum class ExprKind {
  Constant,
  Variable,
  PrimedVariable,
  Nondet,
  Choose,
  Not,
  And,
  Or,
  Xor,
  Implies,
  Eq,
  Neq,
  Ite
};

struct BooleanExpr {
  ExprKind kind = ExprKind::Constant;
  bool constant = false;
  std::string name;
  std::vector<BooleanExpr> operands;

  static BooleanExpr makeConstant(bool value);
  static BooleanExpr makeVariable(std::string name, bool primed = false);
  static BooleanExpr makeNondet();
  static BooleanExpr makeChoose(BooleanExpr lhs, BooleanExpr rhs);
  static BooleanExpr makeUnary(ExprKind kind, BooleanExpr operand);
  static BooleanExpr makeBinary(ExprKind kind, BooleanExpr lhs, BooleanExpr rhs);
  static BooleanExpr makeTernary(BooleanExpr cond, BooleanExpr then_expr,
                                 BooleanExpr else_expr);
};

struct SourceNote {
  std::string file;
  unsigned line = 0;
  std::string function;
};

enum class StatementKind {
  Skip,
  Assume,
  Assert,
  Assign,
  Goto,
  Branch,
  If,
  While,
  Call,
  Return,
  Print,
  Sync,
  StartThread,
  EndThread,
  AtomicBegin,
  AtomicEnd,
  Dead
};

struct AssignmentTarget {
  std::string name;
};

struct AssignmentRhs {
  BooleanExpr value;
};

struct AssignmentStmt {
  std::vector<AssignmentTarget> lhs;
  std::vector<AssignmentRhs> rhs;
  std::optional<BooleanExpr> constraint;
  std::string call_callee;
  std::vector<BooleanExpr> call_args;
};

struct Statement {
  StatementKind kind = StatementKind::Skip;
  std::string label;
  std::vector<std::string> aliases;
  SourceNote source;
  std::string comment;

  BooleanExpr expr;
  AssignmentStmt assignment;
  std::vector<std::string> targets;
  std::string thread_target;
  std::string callee;
  std::vector<BooleanExpr> expressions;
  std::vector<Statement> then_statements;
  std::vector<std::pair<BooleanExpr, std::vector<Statement>>> elsif_branches;
  std::vector<Statement> else_statements;
  std::vector<Statement> body_statements;
  std::vector<std::string> dead_variables;
};

struct Procedure {
  std::string name;
  bool returns_bool = false;
  bool dfs = false;
  std::optional<unsigned> bool_width;
  std::vector<std::string> parameters;
  std::vector<std::string> locals;
  std::optional<BooleanExpr> enforce;
  std::optional<BooleanExpr> abortif;
  std::vector<Statement> statements;
};

struct PredicateDecl {
  std::string name;
  std::string comment;
};

struct BooleanProgram {
  std::vector<PredicateDecl> globals;
  std::vector<Procedure> procedures;

  const Procedure *findProcedure(const std::string &name) const;
};

struct FrontendError {
  std::string message;
  unsigned line = 0;
  unsigned column = 0;
};

class FrontendException : public std::runtime_error {
public:
  explicit FrontendException(const FrontendError &error);

  const FrontendError &error() const { return error_; }

private:
  FrontendError error_;
};

enum class LoweredInstructionKind {
  PredicateTransfer,
  Call,
  Print,
  Return,
  Sync,
};

struct LoweredInstruction {
  std::string id;
  LoweredInstructionKind kind = LoweredInstructionKind::PredicateTransfer;
  StatementKind source_kind = StatementKind::Skip;
  std::optional<npa::PredicateRelation> relation;
  std::string callee;
  std::vector<std::string> results;
  std::vector<BooleanExpr> arguments;
};

struct LoweredNode {
  std::string label;
  std::vector<std::string> outgoing_edges;
};

struct LoweredEdge {
  std::string from;
  std::string to;
  std::string instruction_id;
};

struct LoweringResult {
  std::vector<std::string> predicates;
  std::unordered_map<std::string, unsigned> predicate_to_index;
  std::vector<LoweredNode> nodes;
  std::vector<LoweredInstruction> instructions;
  std::vector<LoweredEdge> edges;
  std::string entry_label;
  std::vector<std::string> exit_labels;

  const LoweredNode *findNode(const std::string &label) const;
  const LoweredInstruction *findInstruction(const std::string &id) const;
  const LoweredEdge *findEdge(const std::string &from, const std::string &to) const;
};

} // namespace frontend
} // namespace verification
} // namespace lotus
