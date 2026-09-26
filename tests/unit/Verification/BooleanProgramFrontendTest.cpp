#include "Verification/Frontend/BooleanProgram.h"
#include "Verification/Frontend/BooleanNPAProgram.h"
#include "Verification/Frontend/BooleanProgramParser.h"
#include "Verification/Frontend/PredicateProgramLowering.h"

#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"
#include "Dataflow/NPA/NPA.h"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using lotus::verification::frontend::BooleanProgram;
using lotus::verification::frontend::LoweringResult;
using lotus::verification::frontend::LoweredEdge;
using lotus::verification::frontend::LoweredInstruction;
using lotus::verification::frontend::LoweredInstructionKind;
using lotus::verification::frontend::StatementKind;
using lotus::verification::frontend::parseBooleanProgram;
using lotus::verification::frontend::lowerToPredicateProgram;
using D = npa::PredicateRelationDomain;

std::vector<std::pair<std::uint64_t, std::uint64_t>>
sortedTransitions(const D::value_type &relation) {
  auto transitions = D::materialize(relation);
  std::sort(transitions.begin(), transitions.end());
  return transitions;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>>
sortedTransitions(const LoweredInstruction &instruction) {
  EXPECT_TRUE(instruction.relation.has_value());
  return sortedTransitions(*instruction.relation);
}

const LoweredEdge &findEdge(const LoweringResult &lowered, const std::string &from,
                            const std::string &to) {
  const auto *edge = lowered.findEdge(from, to);
  EXPECT_NE(edge, nullptr);
  return *edge;
}

const LoweredInstruction &instructionForEdge(const LoweringResult &lowered,
                                             const LoweredEdge &edge) {
  const auto *instruction = lowered.findInstruction(edge.instruction_id);
  EXPECT_NE(instruction, nullptr);
  return *instruction;
}

TEST(BooleanProgramFrontend, ParsesSatabsStyleProgram) {
  const char *text = R"BP(
decl b0;
decl b1;

void main() begin
L0: b0,b1 := *,1 constrain ('b0 | 'b1);
L1: if !b1 then goto bad; fi;
L2: assert(!b0 | b1);
bad: skip;
end
)BP";

  BooleanProgram program = parseBooleanProgram(text);

  ASSERT_EQ(program.globals.size(), 2u);
  EXPECT_EQ(program.globals[0].name, "b0");
  EXPECT_EQ(program.globals[1].name, "b1");

  ASSERT_EQ(program.procedures.size(), 1u);
  const auto &procedure = program.procedures.front();
  EXPECT_EQ(procedure.name, "main");
  ASSERT_EQ(procedure.statements.size(), 4u);
  EXPECT_EQ(procedure.statements[0].kind, StatementKind::Assign);
  EXPECT_EQ(procedure.statements[1].kind, StatementKind::Branch);
  EXPECT_EQ(procedure.statements[2].kind, StatementKind::Assert);
  EXPECT_EQ(procedure.statements[3].label, "bad");
}

TEST(BooleanProgramFrontend, ParsesSdvGrammarForms) {
  const char *text = R"BP(
decl g1, g0;
bool unk() begin if (*) then return T; else return F; fi end
void main() begin
L0:
ALIAS
: g0 := schoose[F,F];
end
)BP";
  auto program = parseBooleanProgram(text);
  ASSERT_EQ(program.globals.size(), 2u);
  ASSERT_EQ(program.procedures.size(), 2u);
  ASSERT_EQ(program.procedures[1].statements.size(), 1u);
  EXPECT_EQ(program.procedures[1].statements[0].label, "L0");
  ASSERT_EQ(program.procedures[1].statements[0].aliases.size(), 1u);
  EXPECT_EQ(program.procedures[1].statements[0].aliases[0], "ALIAS");
}

TEST(BooleanProgramFrontend, LowersParallelAssignAndConstraintToRelation) {
  const char *text = R"BP(
decl b0;
decl b1;

void main() begin
L0: b0,b1 := *,* constrain ('b0 | 'b1);
L1: skip;
end
)BP";

  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));
  const auto &edge = findEdge(lowered, "L0", "L1");
  const auto &instruction = instructionForEdge(lowered, edge);

  EXPECT_EQ(sortedTransitions(instruction),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 1}, {0, 2}, {0, 3}, {1, 1}, {1, 2}, {1, 3},
                {2, 1}, {2, 2}, {2, 3}, {3, 1}, {3, 2}, {3, 3}}));
}

TEST(BooleanProgramFrontend, LowersBranchToGuardedTrueAndFalseEdges) {
  const char *text = R"BP(
decl b0;
decl b1;

void main() begin
L0: if !b1 then goto bad; fi;
L1: skip;
bad: skip;
end
)BP";

  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));

  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, findEdge(lowered, "L0", "bad"))),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {1, 1}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, findEdge(lowered, "L0", "L1"))),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {2, 2}, {3, 3}}));
}

TEST(BooleanProgramFrontend, LowersAssertLikeAssumeToSuccessorGuard) {
  const char *text = R"BP(
decl b0;
decl b1;

void main() begin
L0: assert(!b0 | b1);
L1: skip;
end
)BP";

  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));
  const auto &edge = findEdge(lowered, "L0", "L1");
  const auto &instruction = instructionForEdge(lowered, edge);

  EXPECT_EQ(sortedTransitions(instruction),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {2, 2}, {3, 3}}));
}

TEST(BooleanProgramFrontend, ParsesExtendedGrammarConstructs) {
  const char *text = R"BP(
decl b0;
decl b1;

dfs bool<2> helper(x) begin
decl t;
enforce b0 | b1;
abortif !b0;
L0: print(b0, b1);
L1: if (schoose[0, !b1]) then
  call0();
elsif !b0 then
  b0 := helper();
else
  while !b1 do
    sync worker;
    atomic_begin;
    atomic_end;
    return b0, b1;
  od;
fi;
end
)BP";

  auto program = parseBooleanProgram(text);
  ASSERT_EQ(program.procedures.size(), 1u);
  const auto &procedure = program.procedures.front();

  EXPECT_TRUE(procedure.dfs);
  EXPECT_TRUE(procedure.returns_bool);
  ASSERT_TRUE(procedure.bool_width.has_value());
  EXPECT_EQ(*procedure.bool_width, 2u);
  ASSERT_TRUE(procedure.enforce.has_value());
  ASSERT_TRUE(procedure.abortif.has_value());
  ASSERT_EQ(procedure.statements.size(), 2u);
  EXPECT_EQ(procedure.statements[0].kind, StatementKind::Print);

  const auto &if_stmt = procedure.statements[1];
  EXPECT_EQ(if_stmt.kind, StatementKind::If);
  EXPECT_EQ(if_stmt.expr.kind, lotus::verification::frontend::ExprKind::Choose);
  ASSERT_EQ(if_stmt.then_statements.size(), 1u);
  EXPECT_EQ(if_stmt.then_statements[0].kind, StatementKind::Call);
  ASSERT_EQ(if_stmt.elsif_branches.size(), 1u);
  ASSERT_EQ(if_stmt.elsif_branches[0].second.size(), 1u);
  EXPECT_EQ(if_stmt.elsif_branches[0].second[0].assignment.call_callee, "helper");
  ASSERT_EQ(if_stmt.else_statements.size(), 1u);
  EXPECT_EQ(if_stmt.else_statements[0].kind, StatementKind::While);
  ASSERT_EQ(if_stmt.else_statements[0].body_statements.size(), 4u);
  EXPECT_EQ(if_stmt.else_statements[0].body_statements[0].kind, StatementKind::Sync);
  EXPECT_EQ(if_stmt.else_statements[0].body_statements[1].kind,
            StatementKind::AtomicBegin);
  EXPECT_EQ(if_stmt.else_statements[0].body_statements[2].kind,
            StatementKind::AtomicEnd);
  EXPECT_EQ(if_stmt.else_statements[0].body_statements[3].kind,
            StatementKind::Return);
}

TEST(BooleanProgramFrontend, ParsesEscapedIdentifiersAndCallForms) {
  const char *text = R"BP(
decl {global.pred};

void main() begin
decl {local.pred};
L0: {global.pred} := callee();
L1: helper();
L2: dead {local.pred};
end
)BP";

  auto program = parseBooleanProgram(text);
  ASSERT_EQ(program.globals.size(), 1u);
  EXPECT_EQ(program.globals[0].name, "{global.pred}");

  ASSERT_EQ(program.procedures.size(), 1u);
  const auto &procedure = program.procedures.front();
  ASSERT_EQ(procedure.locals.size(), 1u);
  EXPECT_EQ(procedure.locals[0], "{local.pred}");
  ASSERT_EQ(procedure.statements.size(), 3u);
  EXPECT_EQ(procedure.statements[0].kind, StatementKind::Assign);
  EXPECT_EQ(procedure.statements[0].assignment.lhs[0].name, "{global.pred}");
  EXPECT_EQ(procedure.statements[0].assignment.call_callee, "callee");
  EXPECT_EQ(procedure.statements[1].kind, StatementKind::Call);
  EXPECT_EQ(procedure.statements[1].callee, "helper");
  EXPECT_EQ(procedure.statements[2].kind, StatementKind::Dead);
  ASSERT_EQ(procedure.statements[2].dead_variables.size(), 1u);
  EXPECT_EQ(procedure.statements[2].dead_variables[0], "{local.pred}");
}

TEST(BooleanProgramFrontend, LowersStructuredIfIntoFlatPredicateEdges) {
  const char *text = R"BP(
decl b0;
decl b1;

void main() begin
L0: if b0 then
  L1: b1 := 1;
else
  L2: b1 := 0;
fi;
L3: assert(b1 = b0);
end
)BP";

  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));
  ASSERT_FALSE(lowered.entry_label.empty());

  const auto &branch_true = findEdge(lowered, "L0", "L1");
  const auto &branch_false = findEdge(lowered, "L0", "L2");
  const auto &then_exit = findEdge(lowered, "L1", "L3");
  const auto &else_exit = findEdge(lowered, "L2", "L3");

  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, branch_true)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {1, 1}, {3, 3}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, branch_false)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {2, 2}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, then_exit)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 2}, {1, 3}, {2, 2}, {3, 3}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, else_exit)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {1, 1}, {2, 0}, {3, 1}}));
}

TEST(BooleanProgramFrontend, LowersWhileIntoLoopBackAndExitEdges) {
  const char *text = R"BP(
decl b0;
decl b1;

void main() begin
L0: while b0 do
  L1: b0 := 0;
od;
L2: assert(!b0);
end
)BP";

  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));

  const auto &enter_loop = findEdge(lowered, "L0", "L1");
  const auto &leave_loop = findEdge(lowered, "L0", "L2");
  const auto &back_edge = findEdge(lowered, "L1", "L0");

  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, enter_loop)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {1, 1}, {3, 3}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, leave_loop)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {2, 2}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, back_edge)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {1, 0}, {2, 2}, {3, 2}}));
}

TEST(BooleanProgramFrontend, LowersProcedureEnforceAndAbortifGuards) {
  const char *text = R"BP(
decl b0;
decl b1;

void main() begin
enforce b0 | b1;
abortif !b0;
L0: skip;
end
)BP";

  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));

  EXPECT_EQ(lowered.entry_label, "__bp.main.entry");
  EXPECT_NE(lowered.findEdge("__bp.main.entry", "__bp.main.enforce"),
            nullptr);

  const auto &enforce_edge = findEdge(lowered, "__bp.main.enforce", "__bp.main.abortif");
  const auto &continue_edge = findEdge(lowered, "__bp.main.abortif", "L0");
  const auto &abort_edge = findEdge(lowered, "__bp.main.abortif", "__bp.main.abort");

  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, enforce_edge)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {1, 1}, {2, 2}, {3, 3}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, continue_edge)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {1, 1}, {3, 3}}));
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, abort_edge)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {2, 2}}));
}

TEST(BooleanProgramFrontend, PreservesCallMetadataAndLowersReturn) {
  const char *text = R"BP(
decl b0;

bool main() begin
L0: b0 := callee();
L1: helper();
L2: return b0;
end
)BP";

  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));

  const auto &assign_call = findEdge(lowered, "L0", "L1");
  const auto &assign_instruction = instructionForEdge(lowered, assign_call);
  EXPECT_EQ(assign_instruction.kind, LoweredInstructionKind::Call);
  EXPECT_FALSE(assign_instruction.relation.has_value());
  EXPECT_EQ(assign_instruction.callee, "callee");
  ASSERT_EQ(assign_instruction.results.size(), 1u);
  EXPECT_EQ(assign_instruction.results[0], "b0");

  const auto &plain_call = findEdge(lowered, "L1", "L2");
  const auto &plain_instruction = instructionForEdge(lowered, plain_call);
  EXPECT_EQ(plain_instruction.kind, LoweredInstructionKind::Call);
  EXPECT_FALSE(plain_instruction.relation.has_value());
  EXPECT_EQ(plain_instruction.callee, "helper");
  EXPECT_TRUE(plain_instruction.results.empty());

  const auto *ret = lowered.findEdge("L2", lowered.normal_exit_label);
  ASSERT_NE(ret, nullptr);
  const auto *ret_instruction = lowered.findInstruction(ret->instruction_id);
  ASSERT_NE(ret_instruction, nullptr);
  EXPECT_EQ(ret_instruction->kind, LoweredInstructionKind::Return);
  EXPECT_TRUE(ret_instruction->relation.has_value());
  ASSERT_EQ(ret_instruction->arguments.size(), 1u);
  EXPECT_EQ(ret_instruction->arguments[0].kind,
            lotus::verification::frontend::ExprKind::Variable);
  EXPECT_EQ(ret_instruction->arguments[0].name, "b0");
  const auto *ret_node = lowered.findNode("L2");
  ASSERT_NE(ret_node, nullptr);
  EXPECT_EQ(ret_node->outgoing_edges.size(), 1u);
}

TEST(BooleanProgramFrontend, PreservesLastTransferAndSchooseConstraints) {
  const char *text = R"BP(
decl g;
void main() begin
L0: g := schoose[g, !g];
end
)BP";
  auto lowered = lowerToPredicateProgram(parseBooleanProgram(text));
  const auto &edge = findEdge(lowered, "L0", lowered.normal_exit_label);
  EXPECT_EQ(sortedTransitions(instructionForEdge(lowered, edge)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {1, 1}}));

  const char *nondeterministic = R"BP(
decl g;
void main() begin
L0: g := schoose[F, F];
end
)BP";
  auto nondet_lowered =
      lowerToPredicateProgram(parseBooleanProgram(nondeterministic));
  const auto &nondet_edge =
      findEdge(nondet_lowered, "L0", nondet_lowered.normal_exit_label);
  EXPECT_EQ(sortedTransitions(instructionForEdge(nondet_lowered, nondet_edge)),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {0, 1}, {1, 0}, {1, 1}}));
}

TEST(BooleanProgramFrontend, BooleanCallSummariesAgreeAcrossSolvers) {
  const char *text = R"BP(
decl g;
bool flip(x) begin
  decl t;
  L0: t := !x;
  L1: return t;
end
void main() begin
  decl a;
  L0: a := 0;
  L1: g := flip(a);
  L2: assert(g);
  L3: return;
end
)BP";
  auto program = lotus::verification::frontend::buildBooleanNPAProgram(
      parseBooleanProgram(text));
  auto kleene = npa::KleeneSolver<D>::solve(program.equations);
  auto scc = npa::NPASolver<D>::solve(program.equations, false, -1,
                                      npa::LinearStrategy::SCC);
  auto tensor = npa::NPASolver<D>::solve(program.equations, false, -1,
                                         npa::LinearStrategy::TensorProduct);
  ASSERT_TRUE(kleene.second.converged);
  ASSERT_TRUE(scc.second.converged);
  ASSERT_TRUE(tensor.second.converged);
  EXPECT_GT(tensor.second.tensor_rounds, 0);
  ASSERT_EQ(kleene.first.size(), scc.first.size());
  ASSERT_EQ(kleene.first.size(), tensor.first.size());
  for (size_t i = 0; i < kleene.first.size(); ++i) {
    EXPECT_EQ(kleene.first[i].first, scc.first[i].first);
    EXPECT_EQ(kleene.first[i].first, tensor.first[i].first);
    EXPECT_TRUE(D::equal(kleene.first[i].second, scc.first[i].second));
    if (!D::equal(kleene.first[i].second, tensor.first[i].second)) {
      auto expected = sortedTransitions(kleene.first[i].second);
      auto actual = sortedTransitions(tensor.first[i].second);
      EXPECT_EQ(expected, actual) << kleene.first[i].first;
    }
  }
  auto summary_it = std::find_if(
      kleene.first.begin(), kleene.first.end(), [&](const auto &entry) {
        return entry.first == program.summary_symbols[1];
      });
  ASSERT_NE(summary_it, kleene.first.end());
  const auto &main_summary = summary_it->second;
  const auto transitions = D::materialize(main_summary);
  ASSERT_FALSE(transitions.empty());
  for (const auto &transition : transitions)
    EXPECT_NE(transition.second & 1, 0u);
}

TEST(BooleanProgramFrontend, PropagatesAssertionErrorsThroughCalls) {
  const char *text = R"BP(
decl g;
void bad() begin
  L0: assert(F);
end
void main() begin
  L0: bad();
end
)BP";
  auto program = lotus::verification::frontend::buildBooleanNPAProgram(
      parseBooleanProgram(text));
  auto solved = npa::KleeneSolver<D>::solve(program.equations);
  auto find_value = [&](const npa::Symbol &symbol) -> const D::value_type & {
    auto it = std::find_if(solved.first.begin(), solved.first.end(),
                           [&](const auto &entry) { return entry.first == symbol; });
    EXPECT_NE(it, solved.first.end());
    return it->second;
  };
  EXPECT_TRUE(D::equal(find_value(program.summary_symbols[1]), D::zero()));
  EXPECT_FALSE(
      D::equal(find_value(program.error_summary_symbols[1]), D::zero()));
  const auto &bad_nodes = program.lowered.procedures[0].nodes;
  auto error_node = std::find_if(
      bad_nodes.begin(), bad_nodes.end(), [](const auto &node) {
        return node.label == "__bp.bad.error";
      });
  ASSERT_NE(error_node, bad_nodes.end());
  const unsigned error_index =
      static_cast<unsigned>(error_node - bad_nodes.begin());
  EXPECT_FALSE(D::equal(
      D::extend(find_value(program.entry_reach_symbols[0]),
                find_value(program.forward_node_symbols[0][error_index])),
      D::zero()));
}

TEST(BooleanProgramFrontend, RecursiveCallsPreserveCallerLocals) {
  const char *text = R"BP(
decl g;
bool rec(x) begin
  decl saved;
  L0: saved := x;
  L1: if x then
    return saved;
  else
    x := 1;
    g := rec(x);
    return saved;
  fi;
end
void main() begin
  decl a;
  L0: a := 0;
  L1: g := rec(a);
  L2: assert(!g);
end
)BP";
  auto program = lotus::verification::frontend::buildBooleanNPAProgram(
      parseBooleanProgram(text));
  auto kleene = npa::KleeneSolver<D>::solve(program.equations);
  auto scc = npa::NPASolver<D>::solve(program.equations, false, -1,
                                      npa::LinearStrategy::SCC);
  auto tensor = npa::NPASolver<D>::solve(program.equations, false, -1,
                                         npa::LinearStrategy::TensorProduct);
  ASSERT_TRUE(kleene.second.converged);
  ASSERT_TRUE(scc.second.converged);
  ASSERT_TRUE(tensor.second.converged);
  ASSERT_EQ(kleene.first.size(), scc.first.size());
  ASSERT_EQ(kleene.first.size(), tensor.first.size());
  for (size_t i = 0; i < kleene.first.size(); ++i) {
    EXPECT_TRUE(D::equal(kleene.first[i].second, scc.first[i].second))
        << kleene.first[i].first;
    EXPECT_TRUE(D::equal(kleene.first[i].second, tensor.first[i].second))
        << kleene.first[i].first;
  }
  auto find_value = [&](const npa::Symbol &symbol) -> const D::value_type & {
    auto it = std::find_if(kleene.first.begin(), kleene.first.end(),
                           [&](const auto &item) { return item.first == symbol; });
    EXPECT_NE(it, kleene.first.end());
    return it->second;
  };
  EXPECT_FALSE(D::equal(find_value(program.summary_symbols[1]), D::zero()));
  EXPECT_TRUE(
      D::equal(find_value(program.error_summary_symbols[1]), D::zero()));
}

TEST(BooleanProgramFrontend, MultiBitReturnsSupportDiscardTargets) {
  const char *text = R"BP(
decl g;
bool<2> pair(x) begin
  L0: return x, !x;
end
void main() begin
  L0: g, _ := pair(1);
  L1: assert(g);
end
)BP";
  auto program = lotus::verification::frontend::buildBooleanNPAProgram(
      parseBooleanProgram(text));
  auto kleene = npa::KleeneSolver<D>::solve(program.equations);
  auto tensor = npa::NPASolver<D>::solve(program.equations, false, -1,
                                         npa::LinearStrategy::TensorProduct);
  ASSERT_TRUE(kleene.second.converged);
  ASSERT_TRUE(tensor.second.converged);
  ASSERT_EQ(kleene.first.size(), tensor.first.size());
  for (size_t i = 0; i < kleene.first.size(); ++i)
    EXPECT_TRUE(D::equal(kleene.first[i].second, tensor.first[i].second))
        << kleene.first[i].first;
  auto error_it = std::find_if(
      kleene.first.begin(), kleene.first.end(), [&](const auto &item) {
        return item.first == program.error_summary_symbols[1];
      });
  ASSERT_NE(error_it, kleene.first.end());
  EXPECT_TRUE(D::equal(error_it->second, D::zero()));
}

TEST(BooleanProgramFrontend, ForwardFactsReachLabelsWithInfeasibleSuffixes) {
  const char *text = R"BP(
decl g;
void main() begin
  L0: g := 0;
  TARGET: g := 1 constrain (F);
end
)BP";
  auto program = lotus::verification::frontend::buildBooleanNPAProgram(
      parseBooleanProgram(text));
  auto solved = npa::KleeneSolver<D>::solve(program.equations);
  const auto &nodes = program.lowered.procedures[0].nodes;
  auto node_it = std::find_if(nodes.begin(), nodes.end(),
                             [](const auto &node) { return node.label == "TARGET"; });
  ASSERT_NE(node_it, nodes.end());
  const unsigned index = static_cast<unsigned>(node_it - nodes.begin());
  auto find_value = [&](const npa::Symbol &symbol) -> const D::value_type & {
    auto it = std::find_if(solved.first.begin(), solved.first.end(),
                           [&](const auto &item) { return item.first == symbol; });
    EXPECT_NE(it, solved.first.end());
    return it->second;
  };
  EXPECT_FALSE(D::equal(
      find_value(program.forward_node_symbols[0][index]), D::zero()));
  EXPECT_TRUE(
      D::equal(find_value(program.node_symbols[0][index]), D::zero()));
}

TEST(BooleanProgramFrontend, NondeterministicConditionsTakeBothOutcomes) {
  const char *text = R"BP(
decl g;
bool unk() begin
  L0: if (*) then
    return T;
  else
    return F;
  fi;
end
void main() begin
  L0: g := unk();
  L1: assert(*);
end
)BP";
  auto program = lotus::verification::frontend::buildBooleanNPAProgram(
      parseBooleanProgram(text));
  auto solved = npa::KleeneSolver<D>::solve(program.equations);
  auto find_value = [&](const npa::Symbol &symbol) -> const D::value_type & {
    auto it = std::find_if(solved.first.begin(), solved.first.end(),
                           [&](const auto &item) { return item.first == symbol; });
    EXPECT_NE(it, solved.first.end());
    return it->second;
  };
  EXPECT_FALSE(D::equal(find_value(program.summary_symbols[1]), D::zero()));
  EXPECT_FALSE(
      D::equal(find_value(program.error_summary_symbols[1]), D::zero()));
  const auto &unk = find_value(program.summary_symbols[0]);
  auto transitions = sortedTransitions(unk);
  bool returns_false = false;
  bool returns_true = false;
  const unsigned return_bit = program.lowered.layout.return_begin;
  for (const auto &transition : transitions) {
    returns_false |= (transition.second & (1u << return_bit)) == 0;
    returns_true |= (transition.second & (1u << return_bit)) != 0;
  }
  EXPECT_TRUE(returns_false);
  EXPECT_TRUE(returns_true);
}

} // namespace
