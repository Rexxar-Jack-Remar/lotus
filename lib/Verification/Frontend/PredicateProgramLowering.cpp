#include "Verification/Frontend/PredicateProgramLowering.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace lotus {
namespace verification {
namespace frontend {

namespace {

struct ExprLoweringContext {
  const std::unordered_map<std::string, unsigned> &predicate_to_index;
  npa::PredicateVariableVersion version;
};

struct ExprOutcome {
  npa::PredicateFormula can_false;
  npa::PredicateFormula can_true;
};

ExprOutcome lowerExprOutcome(const BooleanExpr &expr, ExprLoweringContext &ctx) {
  using PF = npa::PredicateFormula;
  switch (expr.kind) {
  case ExprKind::Constant:
    return {PF::constant(!expr.constant), PF::constant(expr.constant)};
  case ExprKind::Variable:
  case ExprKind::PrimedVariable: {
    auto it = ctx.predicate_to_index.find(expr.name);
    if (it == ctx.predicate_to_index.end())
      throw std::invalid_argument("unknown predicate: " + expr.name);
    PF variable = PF::variable(it->second,
                               expr.kind == ExprKind::PrimedVariable
                                   ? npa::PredicateVariableVersion::Next
                                   : ctx.version);
    return {PF::negate(variable), variable};
  }
  case ExprKind::Nondet:
    return {PF::constant(true), PF::constant(true)};
  case ExprKind::Choose:
    return lowerExprOutcome(expr.operands[1], ctx);
  case ExprKind::Not: {
    ExprOutcome operand = lowerExprOutcome(expr.operands[0], ctx);
    return {operand.can_true, operand.can_false};
  }
  case ExprKind::And: {
    ExprOutcome lhs = lowerExprOutcome(expr.operands[0], ctx);
    ExprOutcome rhs = lowerExprOutcome(expr.operands[1], ctx);
    return {PF::disjunction(lhs.can_false, rhs.can_false),
            PF::conjunction(lhs.can_true, rhs.can_true)};
  }
  case ExprKind::Or: {
    ExprOutcome lhs = lowerExprOutcome(expr.operands[0], ctx);
    ExprOutcome rhs = lowerExprOutcome(expr.operands[1], ctx);
    return {PF::conjunction(lhs.can_false, rhs.can_false),
            PF::disjunction(lhs.can_true, rhs.can_true)};
  }
  case ExprKind::Xor: {
    ExprOutcome lhs = lowerExprOutcome(expr.operands[0], ctx);
    ExprOutcome rhs = lowerExprOutcome(expr.operands[1], ctx);
    return {PF::disjunction(PF::conjunction(lhs.can_true, rhs.can_true),
                            PF::conjunction(lhs.can_false, rhs.can_false)),
            PF::disjunction(PF::conjunction(lhs.can_true, rhs.can_false),
                            PF::conjunction(lhs.can_false, rhs.can_true))};
  }
  case ExprKind::Implies: {
    ExprOutcome lhs = lowerExprOutcome(expr.operands[0], ctx);
    ExprOutcome rhs = lowerExprOutcome(expr.operands[1], ctx);
    return {PF::conjunction(lhs.can_true, rhs.can_false),
            PF::disjunction(lhs.can_false, rhs.can_true)};
  }
  case ExprKind::Eq: {
    ExprOutcome lhs = lowerExprOutcome(expr.operands[0], ctx);
    ExprOutcome rhs = lowerExprOutcome(expr.operands[1], ctx);
    return {PF::disjunction(PF::conjunction(lhs.can_true, rhs.can_false),
                            PF::conjunction(lhs.can_false, rhs.can_true)),
            PF::disjunction(PF::conjunction(lhs.can_true, rhs.can_true),
                            PF::conjunction(lhs.can_false, rhs.can_false))};
  }
  case ExprKind::Neq: {
    ExprOutcome lhs = lowerExprOutcome(expr.operands[0], ctx);
    ExprOutcome rhs = lowerExprOutcome(expr.operands[1], ctx);
    return {PF::disjunction(PF::conjunction(lhs.can_true, rhs.can_true),
                            PF::conjunction(lhs.can_false, rhs.can_false)),
            PF::disjunction(PF::conjunction(lhs.can_true, rhs.can_false),
                            PF::conjunction(lhs.can_false, rhs.can_true))};
  }
  case ExprKind::Ite: {
    ExprOutcome cond = lowerExprOutcome(expr.operands[0], ctx);
    ExprOutcome then_expr = lowerExprOutcome(expr.operands[1], ctx);
    ExprOutcome else_expr = lowerExprOutcome(expr.operands[2], ctx);
    return {PF::disjunction(PF::conjunction(cond.can_true, then_expr.can_false),
                            PF::conjunction(cond.can_false,
                                            else_expr.can_false)),
            PF::disjunction(PF::conjunction(cond.can_true, then_expr.can_true),
                            PF::conjunction(cond.can_false,
                                            else_expr.can_true))};
  }
  }
  throw std::invalid_argument("unsupported expression kind");
}

class LoweringBuilder {
public:
  LoweringBuilder(const BooleanProgram &program, const Procedure &procedure)
      : program_(program), procedure_(procedure) {
    for (size_t i = 0; i < program_.globals.size(); ++i) {
      result_.predicates.push_back(program_.globals[i].name);
      result_.predicate_to_index.emplace(program_.globals[i].name,
                                         static_cast<unsigned>(i));
    }
    if (result_.predicates.empty()) {
      throw std::invalid_argument(
          "Boolean program must declare at least one predicate");
    }
    npa::PredicateRelationDomain::configure(
        static_cast<unsigned>(result_.predicates.size()));
    registerStatementList(procedure_.statements, "__bp." + procedure_.name);
  }

  LoweringResult build() {
    auto entry = lowerStatementList(procedure_.statements, std::nullopt);

    const std::string prefix = "__bp." + procedure_.name;
    if (procedure_.abortif.has_value()) {
      const std::string gate = prefix + ".abortif";
      const std::string abort_exit = prefix + ".abort";
      auto abort_formula = lowerExprToPredicateFormula(
          *procedure_.abortif, result_.predicate_to_index);
      emitPredicateEdge(gate, abort_exit,
                        npa::PredicateRelationDomain::guard(abort_formula),
                        StatementKind::Branch);
      result_.exit_labels.push_back(abort_exit);
      emitPredicateToOptional(
          gate, entry,
          npa::PredicateRelationDomain::guard(
              npa::PredicateFormula::negate(abort_formula)),
          StatementKind::Branch);
      entry = gate;
    }

    if (procedure_.enforce.has_value()) {
      const std::string gate = prefix + ".enforce";
      auto formula =
          lowerExprToPredicateFormula(*procedure_.enforce, result_.predicate_to_index);
      emitPredicateToOptional(gate, entry,
                              npa::PredicateRelationDomain::guard(formula),
                              StatementKind::Assume);
      entry = gate;
    }

    if (entry.has_value())
      result_.entry_label = *entry;

    std::sort(result_.exit_labels.begin(), result_.exit_labels.end());
    result_.exit_labels.erase(
        std::unique(result_.exit_labels.begin(), result_.exit_labels.end()),
        result_.exit_labels.end());
    return result_;
  }

private:
  void registerStatementList(const std::vector<Statement> &statements,
                             const std::string &prefix) {
    for (size_t i = 0; i < statements.size(); ++i) {
      const Statement &stmt = statements[i];
      const std::string primary =
          stmt.label.empty() ? prefix + "." + std::to_string(i) : stmt.label;
      labels_[&stmt] = primary;
      canonical_labels_[primary] = primary;
      for (const std::string &alias : stmt.aliases)
        canonical_labels_[alias] = primary;
      registerStatementList(stmt.then_statements, primary + ".then");
      for (size_t j = 0; j < stmt.elsif_branches.size(); ++j) {
        registerStatementList(stmt.elsif_branches[j].second,
                              primary + ".elsif." + std::to_string(j));
      }
      registerStatementList(stmt.else_statements, primary + ".else");
      registerStatementList(stmt.body_statements, primary + ".body");
    }
  }

  std::string labelFor(const Statement &stmt) const { return labels_.at(&stmt); }

  std::string resolveLabel(const std::string &label) const {
    auto it = canonical_labels_.find(label);
    return it == canonical_labels_.end() ? label : it->second;
  }

  void ensureNode(const std::string &label) {
    if (node_index_.count(label))
      return;
    node_index_[label] = result_.nodes.size();
    result_.nodes.push_back({label, {}});
  }

  std::string nextInstructionId(const std::string &from, const std::string &to) {
    return "inst." + std::to_string(next_instruction_id_++) + "." + from + "." + to;
  }

  void appendInstruction(const LoweredInstruction &instruction) {
    result_.instructions.push_back(instruction);
  }

  void appendEdge(const std::string &from, const std::string &to,
                  const std::string &instruction_id) {
    ensureNode(from);
    ensureNode(to);
    result_.edges.push_back({from, to, instruction_id});
    result_.nodes[node_index_.at(from)].outgoing_edges.push_back(instruction_id);
  }

  void emitPredicateEdge(const std::string &from, const std::string &to,
                         const npa::PredicateRelation &relation,
                         StatementKind kind) {
    LoweredInstruction instruction;
    instruction.id = nextInstructionId(from, to);
    instruction.kind = LoweredInstructionKind::PredicateTransfer;
    instruction.relation = relation;
    instruction.source_kind = kind;
    appendInstruction(instruction);
    appendEdge(from, to, instruction.id);
  }

  void emitCallEdge(const std::string &from, const std::string &to,
                    const std::string &callee,
                    const std::vector<std::string> &results,
                    const std::vector<BooleanExpr> &arguments,
                    StatementKind kind) {
    LoweredInstruction instruction;
    instruction.id = nextInstructionId(from, to);
    instruction.kind = LoweredInstructionKind::Call;
    instruction.source_kind = kind;
    instruction.callee = callee;
    instruction.results = results;
    instruction.arguments = arguments;
    appendInstruction(instruction);
    appendEdge(from, to, instruction.id);
  }

  void emitSimpleInstructionEdge(const std::string &from, const std::string &to,
                                 LoweredInstructionKind instruction_kind,
                                 StatementKind kind) {
    LoweredInstruction instruction;
    instruction.id = nextInstructionId(from, to);
    instruction.kind = instruction_kind;
    instruction.source_kind = kind;
    appendInstruction(instruction);
    appendEdge(from, to, instruction.id);
  }

  void emitPredicateToOptional(const std::string &from,
                               const std::optional<std::string> &to,
                               const npa::PredicateRelation &relation,
                               StatementKind kind) {
    if (to.has_value())
      emitPredicateEdge(from, *to, relation, kind);
    else
      result_.exit_labels.push_back(from);
  }

  std::optional<std::string>
  lowerStatementList(const std::vector<Statement> &statements,
                     const std::optional<std::string> &continuation) {
    std::optional<std::string> next = continuation;
    for (auto it = statements.rbegin(); it != statements.rend(); ++it)
      next = lowerStatement(*it, next);
    return next;
  }

  std::optional<std::string>
  lowerStatement(const Statement &stmt,
                 const std::optional<std::string> &continuation) {
    const std::string entry = labelFor(stmt);

    switch (stmt.kind) {
    case StatementKind::Skip:
    case StatementKind::StartThread:
    case StatementKind::EndThread:
    case StatementKind::AtomicBegin:
    case StatementKind::AtomicEnd:
    case StatementKind::Dead:
      emitPredicateToOptional(entry, continuation,
                              npa::PredicateRelationDomain::one(), stmt.kind);
      return entry;

    case StatementKind::Print:
      if (continuation.has_value()) {
        emitSimpleInstructionEdge(entry, *continuation,
                                  LoweredInstructionKind::Print,
                                  stmt.kind);
      } else {
        result_.exit_labels.push_back(entry);
        ensureNode(entry);
      }
      return entry;

    case StatementKind::Sync:
      if (continuation.has_value()) {
        emitSimpleInstructionEdge(entry, *continuation,
                                  LoweredInstructionKind::Sync,
                                  stmt.kind);
      } else {
        result_.exit_labels.push_back(entry);
        ensureNode(entry);
      }
      return entry;

    case StatementKind::Call:
      if (continuation.has_value()) {
        emitCallEdge(entry, *continuation, stmt.callee, {}, stmt.expressions,
                     stmt.kind);
      } else {
        result_.exit_labels.push_back(entry);
        ensureNode(entry);
      }
      return entry;

    case StatementKind::Return:
      {
        ensureNode(entry);
        LoweredInstruction instruction;
        instruction.id = nextInstructionId(entry, entry);
        instruction.kind = LoweredInstructionKind::Return;
        instruction.source_kind = stmt.kind;
        instruction.arguments = stmt.expressions;
        appendInstruction(instruction);
      }
      result_.exit_labels.push_back(entry);
      return entry;

    case StatementKind::Goto:
      for (const std::string &target : stmt.targets) {
        emitPredicateEdge(entry, resolveLabel(target),
                          npa::PredicateRelationDomain::one(), stmt.kind);
      }
      return entry;

    case StatementKind::Assume:
    case StatementKind::Assert: {
      auto formula =
          lowerExprToPredicateFormula(stmt.expr, result_.predicate_to_index);
      emitPredicateToOptional(entry, continuation,
                              npa::PredicateRelationDomain::guard(formula),
                              stmt.kind);
      return entry;
    }

    case StatementKind::Branch: {
      auto condition =
          lowerExprToPredicateFormula(stmt.expr, result_.predicate_to_index);
      emitPredicateEdge(entry, resolveLabel(stmt.targets.front()),
                        npa::PredicateRelationDomain::guard(condition),
                        stmt.kind);
      emitPredicateToOptional(
          entry, continuation,
          npa::PredicateRelationDomain::guard(
              npa::PredicateFormula::negate(condition)),
          stmt.kind);
      return entry;
    }

    case StatementKind::Assign: {
      if (!stmt.assignment.call_callee.empty()) {
        std::vector<std::string> results;
        results.reserve(stmt.assignment.lhs.size());
        for (const auto &target : stmt.assignment.lhs)
          results.push_back(target.name);
        if (continuation.has_value()) {
          emitCallEdge(entry, *continuation, stmt.assignment.call_callee, results,
                       stmt.assignment.call_args, stmt.kind);
        } else {
          result_.exit_labels.push_back(entry);
          ensureNode(entry);
        }
        return entry;
      }

      std::vector<npa::PredicateUpdate> updates;
      updates.reserve(stmt.assignment.lhs.size());
      for (size_t i = 0; i < stmt.assignment.lhs.size(); ++i) {
        const auto &target = stmt.assignment.lhs[i];
        auto it = result_.predicate_to_index.find(target.name);
        if (it == result_.predicate_to_index.end()) {
          throw std::invalid_argument("assignment to unknown predicate: " +
                                      target.name);
        }

        ExprLoweringContext ctx{result_.predicate_to_index,
                                npa::PredicateVariableVersion::Current};
        ExprOutcome outcome =
            lowerExprOutcome(stmt.assignment.rhs.at(i).value, ctx);
        updates.push_back({it->second, outcome.can_false, outcome.can_true});
      }

      std::optional<npa::PredicateFormula> constraint;
      if (stmt.assignment.constraint.has_value()) {
        constraint = lowerExprToPredicateFormula(
            *stmt.assignment.constraint, result_.predicate_to_index,
            npa::PredicateVariableVersion::Next);
      }

      emitPredicateToOptional(
          entry, continuation,
          npa::PredicateRelationDomain::parallelAssign(updates, constraint),
          stmt.kind);
      return entry;
    }

    case StatementKind::If: {
      std::optional<std::string> false_target =
          lowerStatementList(stmt.else_statements, continuation);

      for (size_t idx = stmt.elsif_branches.size(); idx > 0; --idx) {
        const size_t branch_index = idx - 1;
        const std::string cond_label =
            entry + ".elsifcond." + std::to_string(branch_index);
        auto branch_entry =
            lowerStatementList(stmt.elsif_branches[branch_index].second,
                               continuation);
        auto branch_cond = lowerExprToPredicateFormula(
            stmt.elsif_branches[branch_index].first, result_.predicate_to_index);
        emitPredicateToOptional(cond_label, branch_entry,
                                npa::PredicateRelationDomain::guard(branch_cond),
                                stmt.kind);
        emitPredicateToOptional(
            cond_label, false_target,
            npa::PredicateRelationDomain::guard(
                npa::PredicateFormula::negate(branch_cond)),
            stmt.kind);
        false_target = cond_label;
      }

      auto then_entry = lowerStatementList(stmt.then_statements, continuation);
      auto condition =
          lowerExprToPredicateFormula(stmt.expr, result_.predicate_to_index);
      emitPredicateToOptional(entry, then_entry,
                              npa::PredicateRelationDomain::guard(condition),
                              stmt.kind);
      emitPredicateToOptional(
          entry, false_target,
          npa::PredicateRelationDomain::guard(
              npa::PredicateFormula::negate(condition)),
          stmt.kind);
      return entry;
    }

    case StatementKind::While: {
      std::optional<std::string> body_entry =
          lowerStatementList(stmt.body_statements, entry);
      if (!body_entry.has_value())
        body_entry = entry;

      auto condition =
          lowerExprToPredicateFormula(stmt.expr, result_.predicate_to_index);
      emitPredicateToOptional(entry, body_entry,
                              npa::PredicateRelationDomain::guard(condition),
                              stmt.kind);
      emitPredicateToOptional(
          entry, continuation,
          npa::PredicateRelationDomain::guard(
              npa::PredicateFormula::negate(condition)),
          stmt.kind);
      return entry;
    }
    }

    throw std::invalid_argument("unsupported statement kind in lowering");
  }

  const BooleanProgram &program_;
  const Procedure &procedure_;
  LoweringResult result_;
  std::unordered_map<const Statement *, std::string> labels_;
  std::unordered_map<std::string, std::string> canonical_labels_;
  std::unordered_map<std::string, size_t> node_index_;
  size_t next_instruction_id_ = 0;
};

} // namespace

npa::PredicateFormula lowerExprToPredicateFormula(
    const BooleanExpr &expr,
    const std::unordered_map<std::string, unsigned> &predicate_to_index,
    npa::PredicateVariableVersion version) {
  ExprLoweringContext ctx{predicate_to_index, version};
  return lowerExprOutcome(expr, ctx).can_true;
}

LoweringResult lowerToPredicateProgram(const BooleanProgram &program,
                                       const std::string &procedure_name) {
  const Procedure *procedure = program.findProcedure(procedure_name);
  if (!procedure)
    throw std::invalid_argument("procedure not found: " + procedure_name);
  return LoweringBuilder(program, *procedure).build();
}

} // namespace frontend
} // namespace verification
} // namespace lotus
