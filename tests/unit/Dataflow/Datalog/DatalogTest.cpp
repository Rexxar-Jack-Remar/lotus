#include "Dataflow/Datalog/Datalog.h"

#include <atomic>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

using namespace lotus::datalog;

namespace {

template <typename... Ts>
std::set<std::tuple<Ts...>> asSet(const Relation<Ts...> &relation) {
  const auto rows = relation.rows();
  return {rows.begin(), rows.end()};
}

TEST(DatalogTest, EvaluatesNonRecursiveTypedRule) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");

  edge.insert(1, 2);
  edge.insert(2, 3);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(path), (std::set<std::tuple<int, int>>{{1, 2}, {2, 3}}));
}

TEST(DatalogTest, ComputesTransitiveClosureWithSemiNaiveRecursion) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");

  edge.insert(1, 2);
  edge.insert(2, 3);
  edge.insert(3, 4);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(path), (std::set<std::tuple<int, int>>{
                             {1, 2}, {1, 3}, {1, 4}, {2, 3}, {2, 4}, {3, 4}}));
  EXPECT_GE(compiled.stats().fixpoint_iterations, 1U);
  EXPECT_GE(compiled.stats().index_lookups, 1U);
}

TEST(DatalogTest, HandlesMultipleRecursiveAtomsInOneRule) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");

  edge.insert(1, 2);
  edge.insert(2, 3);
  edge.insert(3, 4);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && path(y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(path.contains(1, 4));
  EXPECT_EQ(path.rows().size(), 6U);
}

TEST(DatalogTest, UsesExtensionalFactsAsInitialRecursiveDelta) {
  context ctx;
  auto number = ctx.relation<int>("number");
  auto x = ctx.var<int>("x");

  number.insert(0);
  program p(ctx);
  p.rule(number(x + 1), number(x) && where(x < 3));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(number), (std::set<std::tuple<int>>{{0}, {1}, {2}, {3}}));
}

TEST(DatalogTest, OrdersAcyclicSccsIndependentlyOfRuleDeclarationOrder) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto middle = ctx.relation<int>("middle");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  source.insert(9);
  program p(ctx);
  p.rule(result(x), middle(x));
  p.rule(middle(x), source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.contains(9));
}

TEST(DatalogTest, EvaluatesMutuallyRecursiveScc) {
  context ctx;
  auto seed = ctx.relation<int>("seed");
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto x = ctx.var<int>("x");

  seed.insert(7);
  program p(ctx);
  p.rule(a(x), seed(x));
  p.rule(b(x), a(x));
  p.rule(a(x), b(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(a.contains(7));
  EXPECT_TRUE(b.contains(7));
}

TEST(DatalogTest, EnforcesConstantsAndRepeatedVariableEquality) {
  context ctx;
  auto pair = ctx.relation<int, int>("pair");
  auto diagonal = ctx.relation<int>("diagonal");
  auto selected = ctx.relation<int>("selected");
  auto x = ctx.var<int>("x");

  pair.insert(1, 1);
  pair.insert(1, 2);
  pair.insert(2, 2);

  program p(ctx);
  p.rule(diagonal(x), pair(x, x));
  p.rule(selected(x), pair(1, x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(diagonal), (std::set<std::tuple<int>>{{1}, {2}}));
  EXPECT_EQ(asSet(selected), (std::set<std::tuple<int>>{{1}, {2}}));
}

TEST(DatalogTest, SupportsConditionsAndHeadExpressions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");

  input.insert(1);
  input.insert(2);
  input.insert(3);

  program p(ctx);
  p.rule(output(x + 10), input(x) && where(x >= 2));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{12}, {13}}));
}

TEST(DatalogTest, WildcardsAreFreshAndDoNotImposeEquality) {
  context ctx;
  auto triple = ctx.relation<int, int, int>("triple");
  auto projected = ctx.relation<int>("projected");
  auto x = ctx.var<int>("x");

  triple.insert(1, 2, 3);
  triple.insert(4, 5, 6);

  program p(ctx);
  p.rule(projected(x), triple(x, _, _));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(projected), (std::set<std::tuple<int>>{{1}, {4}}));
}

TEST(DatalogTest, DeduplicatesFacts) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  source.insert(1);
  source.insert(1);
  program p(ctx);
  p.rule(result(x), source(x));
  p.rule(result(x), source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(result.rows().size(), 1U);
}

TEST(DatalogTest, RejectsUngroundedHeadVariableAtCompileTime) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int, int>("result");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");

  program p(ctx);
  p.rule(result(x, y), source(x));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, RejectsUngroundedFilterVariableAtCompileTime) {
  context ctx;
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(result(x), where(x > 0));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, PlannerMovesGroundingAtomBeforeFilter) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  source.insert(1);

  program p(ctx);
  p.rule(result(x), where(x > 0) && source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.contains(1));
}

TEST(DatalogTest, ReRunningACompiledProgramIsIdempotent) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");

  edge.insert(1, 2);
  edge.insert(2, 3);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();
  const auto first = asSet(path);
  compiled.run();

  EXPECT_EQ(asSet(path), first);
  EXPECT_EQ(compiled.stats().inserted_facts, 0U);
}

TEST(DatalogTest, RejectsCrossContextRules) {
  context first;
  context second;
  auto lhs = first.relation<int>("lhs");
  auto rhs = second.relation<int>("rhs");
  auto x = first.var<int>("x");
  auto y = second.var<int>("y");

  program p(first);
  EXPECT_THROW(p.rule(lhs(x), rhs(y)), std::invalid_argument);
}

TEST(DatalogTest, SupportsStringColumnsWithoutRuntimeTypeDispatchInApi) {
  context ctx;
  auto person = ctx.relation<int, std::string>("person");
  auto named = ctx.relation<int>("named");
  auto id = ctx.var<int>("id");

  person.insert(1, "alice");
  person.insert(2, "bob");
  program p(ctx);
  p.rule(named(id), person(id, "alice"));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(named), (std::set<std::tuple<int>>{{1}}));
}

TEST(DatalogTest, ConstantHeadFactsAreDeduplicated) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto answer = ctx.relation<int>("answer");
  source.insert(1);
  source.insert(2);

  program p(ctx);
  p.rule(answer(42), source(_));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(answer), (std::set<std::tuple<int>>{{42}}));
}

TEST(DatalogTest, SupportsCompoundBooleanConditions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  for (int value = 0; value < 5; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(x), input(x) && where((x > 1) && (x < 4)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{2}, {3}}));
}

TEST(DatalogTest, SupportsNestedArithmeticHeadExpressions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(3);

  program p(ctx);
  p.rule(output((x * 2) + 1), input(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(7));
}

TEST(DatalogTest, SupportsScalarOnLeftSideOfExpression) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(4);

  program p(ctx);
  p.rule(output(10 - x), input(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(6));
}

TEST(DatalogTest, ComputesClosureForCyclicInputGraph) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);
  edge.insert(2, 1);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(path.rows().size(), 4U);
  EXPECT_TRUE(path.contains(1, 1));
  EXPECT_TRUE(path.contains(2, 2));
}

TEST(DatalogTest, IncorporatesNewExtensionalFactsOnLaterRun) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();
  edge.insert(2, 3);
  compiled.run();

  EXPECT_TRUE(path.contains(1, 3));
}

TEST(DatalogTest, EvaluatesThreeRelationRecursiveScc) {
  context ctx;
  auto seed = ctx.relation<int>("seed");
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto c = ctx.relation<int>("c");
  auto x = ctx.var<int>("x");
  seed.insert(5);

  program p(ctx);
  p.rule(a(x), seed(x));
  p.rule(b(x), a(x));
  p.rule(c(x), b(x));
  p.rule(a(x), c(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(a.contains(5));
  EXPECT_TRUE(b.contains(5));
  EXPECT_TRUE(c.contains(5));
}

TEST(DatalogTest, BuildsMultiColumnRuntimeIndexMask) {
  context ctx;
  auto key = ctx.relation<int, int>("key");
  auto triple = ctx.relation<int, int, int>("triple");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  key.insert(1, 3);
  triple.insert(1, 10, 2);
  triple.insert(1, 11, 3);
  triple.insert(2, 12, 3);

  program p(ctx);
  p.rule(output(y), key(x, z) && triple(x, y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{11}}));
  EXPECT_GE(compiled.stats().index_lookups, 1U);
}

TEST(DatalogTest, EmptyInputsProduceNoDerivedFacts) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(result(x), source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.rows().empty());
}

TEST(DatalogTest, MultipleRulesFormSetUnion) {
  context ctx;
  auto left = ctx.relation<int>("left");
  auto right = ctx.relation<int>("right");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  left.insert(1);
  right.insert(2);

  program p(ctx);
  p.rule(result(x), left(x));
  p.rule(result(x), right(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(result), (std::set<std::tuple<int>>{{1}, {2}}));
}

TEST(DatalogTest, FalseConditionSuppressesHeadInsertion) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  source.insert(1);

  program p(ctx);
  p.rule(result(x), source(x) && where(x < 0));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.rows().empty());
}

TEST(DatalogTest, RejectsWildcardInRuleHead) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");

  program p(ctx);
  p.rule(result(_), source(_));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, RejectsComputedTermsInBodyAtoms) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(result(x), source(x + 1));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, RejectsDuplicateRelationNames) {
  context ctx;
  ctx.relation<int>("duplicate");

  EXPECT_THROW(ctx.relation<std::string>("duplicate"), std::invalid_argument);
}

TEST(DatalogTest, EvaluatesStratifiedNegationAsAntiJoin) {
  context ctx;
  auto node = ctx.relation<int>("node");
  auto parent = ctx.relation<int, int>("parent");
  auto root = ctx.relation<int>("root");
  auto x = ctx.var<int>("x");
  node.insert(1);
  node.insert(2);
  node.insert(3);
  parent.insert(1, 2);
  parent.insert(1, 3);

  program p(ctx);
  p.rule(root(x), node(x) && neg(parent(_, x)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(root), (std::set<std::tuple<int>>{{1}}));
}

TEST(DatalogTest, PlannerMovesGroundingAtomBeforeNegation) {
  context ctx;
  auto person = ctx.relation<int>("person");
  auto dead = ctx.relation<int>("dead");
  auto alive = ctx.relation<int>("alive");
  auto x = ctx.var<int>("x");
  person.insert(1);
  person.insert(2);
  dead.insert(2);

  program p(ctx);
  p.rule(alive(x), neg(dead(x)) && person(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(alive), (std::set<std::tuple<int>>{{1}}));
}

TEST(DatalogTest, RejectsUnstratifiableNegativeCycle) {
  context ctx;
  auto universe = ctx.relation<int>("universe");
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(a(x), universe(x) && neg(b(x)));
  p.rule(b(x), universe(x) && neg(a(x)));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, ComputesGroupedMeanAggregate) {
  context ctx;
  auto enrolled = ctx.relation<int>("enrolled");
  auto grade = ctx.relation<int, int, int>("grade");
  auto average = ctx.relation<int, double>("average");
  auto student = ctx.var<int>("student");
  auto score = ctx.var<int>("score");
  auto result = ctx.var<double>("result");
  enrolled.insert(1);
  enrolled.insert(2);
  grade.insert(1, 10, 80);
  grade.insert(1, 11, 100);
  grade.insert(2, 12, 70);

  program p(ctx);
  p.rule(average(student, result),
         enrolled(student) &&
             aggregate(result, mean(score), grade(student, _, score)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(average.contains(1, 90.0));
  EXPECT_TRUE(average.contains(2, 70.0));
}

TEST(DatalogTest, CountAggregateProducesZeroForEmptyGroup) {
  context ctx;
  auto person = ctx.relation<int>("person");
  auto grade = ctx.relation<int, int>("grade");
  auto counts = ctx.relation<int, std::size_t>("counts");
  auto student = ctx.var<int>("student");
  auto count_value = ctx.var<std::size_t>("count");
  person.insert(1);
  person.insert(2);
  grade.insert(1, 90);

  program p(ctx);
  p.rule(counts(student, count_value),
         person(student) && aggregate(count_value, count(), grade(student, _)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(counts.contains(1, 1));
  EXPECT_TRUE(counts.contains(2, 0));
}

TEST(DatalogTest, SupportsGenericBlockingAggregator) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto median = ctx.var<int>("median");
  input.insert(1);
  input.insert(9);
  input.insert(4);

  auto median_aggregator =
      make_aggregator<int>(x, "median", [](const std::vector<int> &values) {
        std::vector<int> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        if (sorted.empty())
          return std::vector<int>{};
        return std::vector<int>{sorted[sorted.size() / 2]};
      });
  program p(ctx);
  p.rule(output(median), aggregate(median, median_aggregator, input(x)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(4));
}

TEST(DatalogTest, RejectsAggregateDependencyCycle) {
  context ctx;
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto x = ctx.var<int>("x");
  auto aggregate_value = ctx.var<int>("aggregate_value");

  program p(ctx);
  p.rule(a(aggregate_value), aggregate(aggregate_value, maximum(x), b(x)));
  p.rule(b(x), a(x));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, LatticeRelationJoinsValuesByKey) {
  context ctx;
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");

  distance.insert(1, 2, min_lattice<int>(100));
  distance.insert(1, 2, min_lattice<int>(70));
  distance.insert(1, 2, min_lattice<int>(90));

  ASSERT_EQ(distance.rows().size(), 1U);
  EXPECT_TRUE(distance.contains(1, 2, min_lattice<int>(70)));
}

TEST(DatalogTest, LatticeSemiNaiveCoalescesCandidatesPerKey) {
  context ctx;
  auto edge = ctx.relation<int, int, int>("edge");
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");
  auto source = ctx.var<int>("source");
  auto middle = ctx.var<int>("middle");
  auto target = ctx.var<int>("target");
  auto weight = ctx.var<int>("weight");
  auto current = ctx.var<min_lattice<int>>("current");
  edge.insert(1, 2, 70);
  edge.insert(1, 2, 50);
  edge.insert(1, 2, 60);
  distance.insert(1, 1, min_lattice<int>(0));

  program p(ctx);
  p.rule(distance(source, target, current + weight),
         distance(source, middle, current) && edge(middle, target, weight));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(distance.contains(1, 2, min_lattice<int>(50)));
  EXPECT_EQ(distance.rows().size(), 2U);
  EXPECT_EQ(compiled.stats().inserted_facts, 1U);
}

TEST(DatalogTest, LatticeShortestPathReachesFixedPoint) {
  context ctx;
  auto edge = ctx.relation<int, int, int>("edge");
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");
  auto source = ctx.var<int>("source");
  auto middle = ctx.var<int>("middle");
  auto target = ctx.var<int>("target");
  auto weight = ctx.var<int>("weight");
  auto current = ctx.var<min_lattice<int>>("current");
  edge.insert(1, 2, 10);
  edge.insert(2, 3, 5);
  edge.insert(1, 3, 30);
  distance.insert(1, 1, min_lattice<int>(0));

  program p(ctx);
  p.rule(distance(source, target, current + weight),
         distance(source, middle, current) && edge(middle, target, weight));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(distance.contains(1, 3, min_lattice<int>(15)));
}

TEST(DatalogTest, GreedyPlannerStartsWithLowerEstimatedCardinality) {
  context ctx;
  auto large = ctx.relation<int, int>("large");
  auto small = ctx.relation<int>("small");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  for (int value = 0; value < 100; ++value)
    large.insert(value, value);
  small.insert(42);

  program p(ctx);
  p.rule(result(x), large(x, y) && small(y));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.contains(42));
  EXPECT_GT(compiled.stats().planned_reorders, 0U);
  EXPECT_GT(compiled.stats().index_lookups, 0U);
}

TEST(DatalogTest, ParallelBspMatchesSerialTransitiveClosure) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  for (int value = 0; value < 40; ++value)
    edge.insert(value, value + 1);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 2;
  compiled.run(options);

  EXPECT_TRUE(path.contains(0, 40));
  EXPECT_EQ(path.rows().size(), 820U);
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
}

TEST(DatalogTest, ParallelLatticeMergeCoalescesAcrossWorkers) {
  context ctx;
  auto edge = ctx.relation<int, int, int>("edge");
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");
  auto source = ctx.var<int>("source");
  auto middle = ctx.var<int>("middle");
  auto target = ctx.var<int>("target");
  auto weight = ctx.var<int>("weight");
  auto current = ctx.var<min_lattice<int>>("current");
  for (int weight_value = 100; weight_value >= 1; --weight_value)
    edge.insert(1, 2, weight_value);
  distance.insert(1, 1, min_lattice<int>(0));

  program p(ctx);
  p.rule(distance(source, target, current + weight),
         distance(source, middle, current) && edge(middle, target, weight));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 1;
  compiled.run(options);

  EXPECT_TRUE(distance.contains(1, 2, min_lattice<int>(1)));
  EXPECT_EQ(distance.rows().size(), 2U);
}

TEST(DatalogTest, SupportsInjectedScheduler) {
  class CountingScheduler final : public Scheduler {
  public:
    std::size_t workerCount() const override { return 2; }
    void
    parallelFor(std::size_t task_count,
                const std::function<void(std::size_t)> &function) override {
      calls += task_count;
      for (std::size_t task = 0; task < task_count; ++task)
        function(task);
    }
    std::size_t calls = 0;
  } scheduler;

  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);
  edge.insert(2, 3);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.scheduler = &scheduler;
  options.parallel_grain_size = 1;
  compiled.run(options);

  EXPECT_TRUE(path.contains(1, 3));
  EXPECT_GT(scheduler.calls, 0U);
}

TEST(DatalogTest, EmitsSccRuleAndDeltaTrace) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);
  edge.insert(2, 3);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  std::ostringstream trace;
  ExecutionOptions options;
  options.trace_scc = true;
  options.trace_rule = true;
  options.trace_delta = true;
  options.trace_stream = &trace;
  compiled.run(options);

  EXPECT_NE(trace.str().find("SCC"), std::string::npos);
  EXPECT_NE(trace.str().find("rule"), std::string::npos);
  EXPECT_NE(trace.str().find("iteration"), std::string::npos);
}

TEST(DatalogTest, SupportsRemainderUnaryAndLiftedExpressions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  for (int value = 1; value <= 4; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(lift([](int value) { return value * value; }, -x)),
         input(x) && where((x % 2) == 0));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{4}, {16}}));
}

TEST(DatalogTest, SupportsMultipleRuleHeads) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto left = ctx.relation<int>("left");
  auto right = ctx.relation<int>("right");
  auto x = ctx.var<int>("x");
  source.insert(7);

  program p(ctx);
  p.rule({left(x), right(x)}, source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(left.contains(7));
  EXPECT_TRUE(right.contains(7));
}

TEST(DatalogTest, SupportsAllBuiltInReducibleAggregates) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto sum_result = ctx.relation<int>("sum_result");
  auto min_result = ctx.relation<int>("min_result");
  auto max_result = ctx.relation<int>("max_result");
  auto count_result = ctx.relation<std::size_t>("count_result");
  auto x = ctx.var<int>("x");
  auto integer_result = ctx.var<int>("integer_result");
  auto size_result = ctx.var<std::size_t>("size_result");
  input.insert(4);
  input.insert(1);
  input.insert(9);

  program p(ctx);
  p.rule(sum_result(integer_result),
         aggregate(integer_result, sum(x), input(x)));
  p.rule(min_result(integer_result),
         aggregate(integer_result, minimum(x), input(x)));
  p.rule(max_result(integer_result),
         aggregate(integer_result, maximum(x), input(x)));
  p.rule(count_result(size_result),
         aggregate(size_result, count(), input(_)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(sum_result.contains(14));
  EXPECT_TRUE(min_result.contains(1));
  EXPECT_TRUE(max_result.contains(9));
  EXPECT_TRUE(count_result.contains(3));
}

TEST(DatalogTest, SupportsMaximumAndSetUnionLattices) {
  context ctx;
  auto maximum = ctx.lattice<int, max_lattice<int>>("maximum");
  auto sets = ctx.lattice<int, set_lattice<int>>("sets");

  maximum.insert(1, max_lattice<int>(3));
  maximum.insert(1, max_lattice<int>(8));
  maximum.insert(1, max_lattice<int>(5));
  sets.insert(1, set_lattice<int>{1, 2});
  sets.insert(1, set_lattice<int>{2, 3});

  EXPECT_TRUE(maximum.contains(1, max_lattice<int>(8)));
  EXPECT_TRUE(sets.contains(1, set_lattice<int>{1, 2, 3}));
  EXPECT_EQ(maximum.rows().size(), 1U);
  EXPECT_EQ(sets.rows().size(), 1U);
}

TEST(DatalogTest, ParallelizesNonRecursiveRuleEvaluation) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  for (int value = 0; value < 1000; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(x + 1), input(x));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 16;
  compiled.run(options);

  EXPECT_EQ(output.rows().size(), 1000U);
  EXPECT_TRUE(output.contains(1000));
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
}

TEST(DatalogTest, ParallelizesReducibleAggregateStates) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto result = ctx.var<int>("result");
  for (int value = 1; value <= 1000; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(result), aggregate(result, sum(x), input(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 16;
  compiled.run(options);

  EXPECT_TRUE(output.contains(500500));
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
}

TEST(DatalogTest, ParallelizesStratifiedNegationDriver) {
  context ctx;
  auto universe = ctx.relation<int>("universe");
  auto blocked = ctx.relation<int>("blocked");
  auto allowed = ctx.relation<int>("allowed");
  auto x = ctx.var<int>("x");
  for (int value = 0; value < 1000; ++value) {
    universe.insert(value);
    if ((value % 2) == 0)
      blocked.insert(value);
  }

  program p(ctx);
  p.rule(allowed(x), universe(x) && neg(blocked(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 16;
  compiled.run(options);

  EXPECT_EQ(allowed.rows().size(), 500U);
  EXPECT_TRUE(allowed.contains(999));
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
}

TEST(DatalogTest, ThreadSchedulerSupportsRepeatedAndNestedBatches) {
  ThreadScheduler scheduler(4);
  std::atomic<std::size_t> executions{0};
  scheduler.parallelFor(100, [&](std::size_t) { ++executions; });
  scheduler.parallelFor(8, [&](std::size_t) {
    scheduler.parallelFor(3, [&](std::size_t) { ++executions; });
  });

  EXPECT_EQ(executions.load(), 124U);
}

} // namespace
