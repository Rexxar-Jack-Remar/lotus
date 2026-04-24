#include "Verification/Frontend/BooleanProgram.h"
#include "Verification/Frontend/BooleanProgramParser.h"
#include "Verification/Frontend/PredicateProgramLowering.h"

#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"

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

  EXPECT_EQ(lowered.entry_label, "__bp.main.enforce");

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

TEST(BooleanProgramFrontend, LeavesCallSemanticsUnresolvedInLoweredCfg) {
  const char *text = R"BP(
decl b0;

void main() begin
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

  const auto *ret = lowered.findEdge("L2", "L2");
  ASSERT_EQ(ret, nullptr);
  const auto *ret_instruction = lowered.findInstruction("inst.0.L2.L2");
  if (ret_instruction == nullptr)
    ret_instruction = lowered.findInstruction("inst.1.L2.L2");
  if (ret_instruction == nullptr)
    ret_instruction = lowered.findInstruction("inst.2.L2.L2");
  ASSERT_NE(ret_instruction, nullptr);
  EXPECT_EQ(ret_instruction->kind, LoweredInstructionKind::Return);
  EXPECT_FALSE(ret_instruction->relation.has_value());
  ASSERT_EQ(ret_instruction->arguments.size(), 1u);
  EXPECT_EQ(ret_instruction->arguments[0].kind,
            lotus::verification::frontend::ExprKind::Variable);
  EXPECT_EQ(ret_instruction->arguments[0].name, "b0");
  const auto *ret_node = lowered.findNode("L2");
  ASSERT_NE(ret_node, nullptr);
  EXPECT_TRUE(ret_node->outgoing_edges.empty());
}

} // namespace
