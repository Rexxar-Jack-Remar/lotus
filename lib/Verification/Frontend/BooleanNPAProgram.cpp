#include "Verification/Frontend/BooleanNPAProgram.h"

#include "Verification/Frontend/PredicateProgramLowering.h"

#include <stdexcept>
#include <unordered_map>

namespace lotus {
namespace verification {
namespace frontend {

namespace {

using D = npa::PredicateRelationDomain;
using E = npa::E0<D>;
using Exp = npa::Exp0<D>;

unsigned returnWidth(const Procedure &procedure) {
  return procedure.returns_bool ? procedure.bool_width.value_or(1) : 0;
}

void rejectConcurrentInstruction(const LoweredInstruction &instruction) {
  switch (instruction.source_kind) {
  case StatementKind::StartThread:
  case StatementKind::EndThread:
  case StatementKind::AtomicBegin:
  case StatementKind::AtomicEnd:
  case StatementKind::Sync:
    throw std::invalid_argument(
        "concurrent Boolean-program statements are unsupported by NPA");
  default:
    break;
  }
}

struct CallTransfer {
  unsigned callee = 0;
  D::value_type pre;
  D::value_type post;
};

CallTransfer buildCallTransfer(
    const LoweredInstruction &instruction, const LoweringResult &caller,
    const PredicateProgramLayout &layout,
    const std::unordered_map<std::string, unsigned> &procedure_to_index,
    const BooleanProgram &program) {
  auto found = procedure_to_index.find(instruction.callee);
  if (found == procedure_to_index.end())
    throw std::invalid_argument("unknown Boolean-program callee: " +
                                instruction.callee);
  const Procedure &callee = program.procedures[found->second];
  if (instruction.arguments.size() != callee.parameters.size())
    throw std::invalid_argument("argument count mismatch in call to " +
                                instruction.callee);
  const unsigned width = returnWidth(callee);
  if (!instruction.results.empty() && instruction.results.size() != width)
    throw std::invalid_argument("return width mismatch in call to " +
                                instruction.callee);

  std::vector<npa::PredicateUpdate> arguments;
  arguments.reserve(instruction.arguments.size());
  for (unsigned i = 0; i < instruction.arguments.size(); ++i) {
    arguments.push_back(lowerExprToPredicateUpdate(
        instruction.arguments[i], caller.predicate_to_index,
        layout.argument_begin + i));
  }
  auto pre = D::parallelAssign(arguments);

  std::vector<npa::PredicateUpdate> results;
  results.reserve(instruction.results.size());
  for (unsigned i = 0; i < instruction.results.size(); ++i) {
    if (instruction.results[i] == "_")
      continue;
    auto target = caller.predicate_to_index.find(instruction.results[i]);
    if (target == caller.predicate_to_index.end())
      throw std::invalid_argument("unknown call result: " +
                                  instruction.results[i]);
    auto value = npa::PredicateFormula::variable(layout.return_begin + i);
    results.push_back({target->second, npa::PredicateFormula::negate(value),
                       value});
  }
  return {found->second, std::move(pre), D::parallelAssign(results)};
}

struct Callsite {
  unsigned caller = 0;
  unsigned callee = 0;
  unsigned source = 0;
  D::value_type pre;
};

} // namespace

BooleanNPAProgram buildBooleanNPAProgram(const BooleanProgram &program,
                                         const std::string &entry_name) {
  if (program.procedures.empty())
    throw std::invalid_argument("Boolean program has no procedures");

  BooleanNPAProgram result;
  result.lowered = lowerBooleanProgram(program);
  const auto &layout = result.lowered.layout;
  const unsigned count = static_cast<unsigned>(program.procedures.size());
  std::unordered_map<std::string, unsigned> procedure_to_index;
  result.summary_symbols.reserve(count);
  result.error_summary_symbols.reserve(count);
  result.entry_reach_symbols.reserve(count);
  result.node_symbols.reserve(count);
  result.error_node_symbols.reserve(count);
  result.forward_node_symbols.reserve(count);

  for (unsigned p = 0; p < count; ++p) {
    const auto &procedure = program.procedures[p];
    if (!procedure_to_index.emplace(procedure.name, p).second)
      throw std::invalid_argument("duplicate procedure: " + procedure.name);
    if (procedure.returns_bool && returnWidth(procedure) == 0)
      throw std::invalid_argument("zero-width Boolean return: " +
                                  procedure.name);
    result.summary_symbols.push_back("bp.summary." + std::to_string(p));
    result.error_summary_symbols.push_back("bp.error_summary." +
                                           std::to_string(p));
    result.entry_reach_symbols.push_back("bp.entry_reach." +
                                         std::to_string(p));
    auto &symbols = result.node_symbols.emplace_back();
    auto &error_symbols = result.error_node_symbols.emplace_back();
    auto &forward_symbols = result.forward_node_symbols.emplace_back();
    for (unsigned n = 0; n < result.lowered.procedures[p].nodes.size(); ++n)
      symbols.push_back("bp.node." + std::to_string(p) + "." +
                        std::to_string(n));
    for (unsigned n = 0; n < result.lowered.procedures[p].nodes.size(); ++n)
      error_symbols.push_back("bp.error_node." + std::to_string(p) + "." +
                              std::to_string(n));
    for (unsigned n = 0; n < result.lowered.procedures[p].nodes.size(); ++n)
      forward_symbols.push_back("bp.forward_node." + std::to_string(p) + "." +
                                std::to_string(n));
  }

  auto entry_procedure = procedure_to_index.find(entry_name);
  if (entry_procedure == procedure_to_index.end())
    throw std::invalid_argument("entry procedure not found: " + entry_name);
  result.entry_procedure = entry_procedure->second;

  std::vector<Callsite> callsites;

  for (unsigned p = 0; p < count; ++p) {
    const auto &lowered = result.lowered.procedures[p];
    std::unordered_map<std::string, unsigned> node_to_index;
    std::unordered_map<std::string, const LoweredInstruction *> instructions;
    for (unsigned n = 0; n < lowered.nodes.size(); ++n)
      node_to_index.emplace(lowered.nodes[n].label, n);
    for (const auto &instruction : lowered.instructions)
      instructions.emplace(instruction.id, &instruction);

    std::vector<E> right_hand_sides(lowered.nodes.size(), Exp::term(D::zero()));
    std::vector<E> error_right_hand_sides(lowered.nodes.size(),
                                          Exp::term(D::zero()));
    std::vector<E> forward_right_hand_sides(lowered.nodes.size(),
                                            Exp::term(D::zero()));
    auto entry = node_to_index.find(lowered.entry_label);
    if (entry == node_to_index.end())
      throw std::invalid_argument("missing entry node in " + lowered.procedure);
    forward_right_hand_sides[entry->second] = Exp::term(D::one());
    for (unsigned n = 0; n < lowered.nodes.size(); ++n) {
      if (lowered.nodes[n].label == lowered.normal_exit_label)
        right_hand_sides[n] = Exp::term(D::one());
      for (const auto &error_label : lowered.error_exit_labels) {
        if (lowered.nodes[n].label == error_label)
          error_right_hand_sides[n] = Exp::term(D::one());
      }
    }
    for (const auto &edge : lowered.edges) {
      auto source = node_to_index.find(edge.from);
      auto target = node_to_index.find(edge.to);
      auto instruction = instructions.find(edge.instruction_id);
      if (source == node_to_index.end() || target == node_to_index.end() ||
          instruction == instructions.end())
        throw std::invalid_argument("invalid lowered Boolean-program edge");
      const auto &inst = *instruction->second;
      rejectConcurrentInstruction(inst);
      E continuation = Exp::hole(result.node_symbols[p][target->second]);
      E error_continuation =
          Exp::hole(result.error_node_symbols[p][target->second]);
      E path;
      E error_path;
      E forward_path;
      if (inst.kind == LoweredInstructionKind::Call) {
        auto call = buildCallTransfer(inst, lowered, layout,
                                      procedure_to_index, program);
        path = Exp::concat(
            Exp::term(call.pre), result.summary_symbols[call.callee],
            Exp::seq(call.post, std::move(continuation)));
        error_path = Exp::ndet(
            Exp::concat(Exp::term(call.pre),
                        result.summary_symbols[call.callee],
                        Exp::seq(call.post, std::move(error_continuation))),
            Exp::concat(Exp::term(call.pre),
                        result.error_summary_symbols[call.callee],
                        Exp::term(D::one())));
        forward_path = Exp::concat(
            Exp::mul(Exp::hole(result.forward_node_symbols[p][source->second]),
                     Exp::term(call.pre)),
            result.summary_symbols[call.callee], Exp::term(call.post));
        callsites.push_back({p, call.callee, source->second, call.pre});
      } else {
        if (inst.kind != LoweredInstructionKind::PredicateTransfer &&
            inst.kind != LoweredInstructionKind::Return &&
            inst.kind != LoweredInstructionKind::Print)
          throw std::invalid_argument("unsupported lowered instruction kind");
        path = Exp::seq(inst.relation.value_or(D::one()),
                        std::move(continuation));
        error_path = Exp::seq(inst.relation.value_or(D::one()),
                              std::move(error_continuation));
        forward_path = Exp::mul(
            Exp::hole(result.forward_node_symbols[p][source->second]),
            Exp::term(inst.relation.value_or(D::one())));
      }
      auto &rhs = right_hand_sides[source->second];
      rhs = Exp::ndet(std::move(rhs), std::move(path));
      auto &error_rhs = error_right_hand_sides[source->second];
      error_rhs = Exp::ndet(std::move(error_rhs), std::move(error_path));
      auto &forward_rhs = forward_right_hand_sides[target->second];
      forward_rhs = Exp::ndet(std::move(forward_rhs),
                              std::move(forward_path));
    }

    for (unsigned n = 0; n < right_hand_sides.size(); ++n)
      result.equations.emplace_back(result.node_symbols[p][n],
                                    std::move(right_hand_sides[n]));
    for (unsigned n = 0; n < error_right_hand_sides.size(); ++n)
      result.equations.emplace_back(result.error_node_symbols[p][n],
                                    std::move(error_right_hand_sides[n]));
    for (unsigned n = 0; n < forward_right_hand_sides.size(); ++n)
      result.equations.emplace_back(result.forward_node_symbols[p][n],
                                    std::move(forward_right_hand_sides[n]));
    result.equations.emplace_back(
        result.summary_symbols[p],
        Exp::project(Exp::hole(result.node_symbols[p][entry->second])));
    result.equations.emplace_back(
        result.error_summary_symbols[p],
        Exp::project(Exp::hole(result.error_node_symbols[p][entry->second])));
  }

  std::vector<E> entry_reach_rhs(count, Exp::term(D::zero()));
  entry_reach_rhs[result.entry_procedure] = Exp::term(D::one());
  for (const auto &call : callsites) {
    auto path = Exp::mul(
        Exp::hole(result.entry_reach_symbols[call.caller]),
        Exp::mul(
            Exp::hole(result.forward_node_symbols[call.caller][call.source]),
            Exp::term(call.pre)));
    auto &rhs = entry_reach_rhs[call.callee];
    rhs = Exp::ndet(std::move(rhs), std::move(path));
  }
  for (unsigned p = 0; p < count; ++p)
    result.equations.emplace_back(result.entry_reach_symbols[p],
                                  std::move(entry_reach_rhs[p]));
  return result;
}

} // namespace frontend
} // namespace verification
} // namespace lotus
