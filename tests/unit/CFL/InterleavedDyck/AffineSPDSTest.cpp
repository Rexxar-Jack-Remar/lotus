#include "AffineSPDSTestSupport.h"

#include <gtest/gtest.h>
TEST(InterleavedDyckAffineSPDS, SharedSuites) {
  for (const auto &test : lotus::cfl::interleaved_dyck::affine::test::tests()) {
    SCOPED_TRACE(test.first);
    EXPECT_NO_THROW(test.second());
  }
}

namespace {
using namespace lotus::cfl::interleaved_dyck;
using namespace lotus::cfl::interleaved_dyck::affine;

TEST(InterleavedDyckAffineSPDS, IntersectionAndCertificateReuse) {
  auto point = Matrix::identity(14);
  auto a = AffineSpace::singleton(point);
  for (std::size_t i = 0; i < 128; ++i) {
    Matrix direction(14);
    direction.set(i / 14, i % 14);
    a.addPoint(point ^ direction);
  }
  auto counters = std::make_shared<AlgebraStatistics>();
  EXPECT_TRUE(a.intersects(a, counters.get()));
  EXPECT_EQ(counters->basis_reductions, 0u);
  EXPECT_EQ(counters->intersection_fast_paths, 1u);
  HistoryComparison comparison(AffineSpace::singleton(Matrix(2)),
                               AffineSpace::singleton(Matrix::identity(2)),
                               counters);
  EXPECT_FALSE(comparison.mayReach());
  const auto reductions = counters->basis_reductions;
  ASSERT_TRUE(comparison.certificate());
  EXPECT_TRUE(comparison.certificate()->verify(comparison.callHistory(),
                                               comparison.fieldHistory()));
  EXPECT_EQ(counters->basis_reductions, reductions);
}

TEST(InterleavedDyckAffineSPDS, DeltaStopsAtFullRankBeforeFallbackProducts) {
  auto counters = std::make_shared<AlgebraStatistics>();
  auto domain = AffineSemiring(2).withStatistics(counters);
  auto full = AffineSpace::top(2);
  auto target = AffineSpace::singleton(Matrix(2));
  for (std::size_t bit = 0; bit < 3; ++bit) {
    Matrix p(2);
    p.set(bit / 2, bit % 2);
    target.addPoint(p);
  }
  Matrix swap(2);
  swap.set(0, 1);
  swap.set(1, 0);
  const auto right = domain.lift(swap);
  AffineDelta input;
  input.pivots = {0, 1, 2, 3};
  AffineDelta output;
  EXPECT_TRUE(
      domain.extendDeltaAndCombine(target, full, right, input, true, &output));
  EXPECT_EQ(target.rank(), 4u);
  EXPECT_EQ(counters->matrix_products, 3u);
  input.full = true;
  const auto products = counters->matrix_products;
  EXPECT_FALSE(domain.extendPushDeltaAndCombine(target, right, full, full,
                                                input, true, &output));
  EXPECT_EQ(counters->matrix_products, products);
}

TEST(InterleavedDyckAffineSPDS, JointBlockCoordinatesMatchDenseAlgebra) {
  std::mt19937 rng(7301);
  for (const auto dimensions :
       {std::pair<std::size_t, std::size_t>{2, 3}, {9, 9}, {65, 2}}) {
    const auto n = dimensions.first + dimensions.second;
    auto layout = std::make_shared<const MatrixLayout>(
        n, std::vector<std::pair<std::size_t, std::size_t>>{
               {0, dimensions.first}, {dimensions.first, dimensions.second}});
    AffineSemiring compact(n, layout), dense(n);
    auto matrix = [&] {
      return Matrix::directSum({test::randomMatrix(dimensions.first, rng),
                                test::randomMatrix(dimensions.second, rng)});
    };
    for (unsigned trial = 0; trial < 6; ++trial) {
      auto a = compact.zero(), b = compact.zero(), ad = dense.zero(),
           bd = dense.zero();
      for (unsigned i = 0; i < 3; ++i) {
        const auto x = matrix(), y = matrix();
        a.addPoint(x);
        ad.addPoint(x);
        b.addPoint(y);
        bd.addPoint(y);
      }
      EXPECT_LT(a.coordinates(), n * n);
      EXPECT_EQ(a, ad);
      EXPECT_EQ(compact.extend(a, b), dense.extend(ad, bd));
      EXPECT_EQ(a.intersects(b), ad.intersects(bd));
      EXPECT_EQ(a.block(dimensions.first, dimensions.second),
                ad.block(dimensions.first, dimensions.second));
      const auto extra = matrix();
      auto incremental = compact.extend(a, b);
      AffineDelta delta, result_delta;
      auto grown = a;
      grown.joinWithDelta(compact.lift(extra), &delta);
      compact.extendDeltaAndCombine(incremental, grown, b, delta, true,
                                    &result_delta);
      EXPECT_EQ(incremental, compact.extend(grown, b));
      const auto fixed = compact.lift(matrix());
      const auto prepared = compact.prepareWeight(fixed);
      EXPECT_EQ(compact.extendPrepared(a, fixed, prepared),
                compact.extend(a, fixed));
    }
    auto left = compact.lift(Matrix(n));
    auto right = compact.lift(Matrix::identity(n));
    auto certificate = separate(left, right);
    ASSERT_TRUE(certificate);
    EXPECT_TRUE(certificate->verify(left, right));
    certificate->functional.set(0, dimensions.first);
    EXPECT_TRUE(certificate->verify(
        left, right)); // Off-block dual entries are irrelevant.
  }
}

TEST(InterleavedDyckAffineSPDS, CompressionPreservesCrossBlockCorrelations) {
  Graph graph;
  graph.addEdge(0, 1, Label::neutral());
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(0, 2, Label::neutral());
  const auto observer =
      HistoryObserver::directSum({HistoryObserver::parity({graph.edges()[0]}),
                                  HistoryObserver::parity({graph.edges()[1]})});
  Options dense_options;
  dense_options.compress_observer = false;
  const auto compact = Solver{}.prepare(graph, observer);
  const auto dense = Solver(dense_options).prepare(graph, observer);
  auto c = compact.queryFrom(0), d = dense.queryFrom(0);
  EXPECT_EQ(c.statistics().coordinate_dimension, 8u);
  EXPECT_EQ(d.statistics().coordinate_dimension, 16u);
  EXPECT_EQ(c.compare(2).callHistory(), d.compare(2).callHistory());
  EXPECT_EQ(c.compare(2).callHistory().rank(), 1u);
  EXPECT_EQ(compact.analyzeAll().pairs, dense.analyzeAll().pairs);
  auto custom = observer;
  Matrix cross = observer.matrix(graph.edges()[0]);
  cross.set(0, 2);
  custom.set(graph.edges()[0], cross);
  EXPECT_FALSE(custom.compactLayout());
  EXPECT_EQ(Solver{}
                .prepare(graph, custom)
                .queryFrom(0)
                .statistics()
                .coordinate_dimension,
            16u);
}

TEST(InterleavedDyckAffineSPDS, SlicesReuseWeightsAndReadoutCaches) {
  Graph graph;
  graph.addEdge(0, 1, Label::neutral());
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(2, 0, Label::neutral());
  graph.addEdge(100, 101, Label::neutral());
  const auto observer = HistoryObserver::parity({graph.edges()[0]});
  Options options;
  options.readout_batch_threshold = 1;
  auto analysis = Solver(options).prepare(graph, observer);
  auto first = analysis.queryFrom(0);
  EXPECT_EQ(first.statistics().prepared_weights, 2u);
  EXPECT_EQ(first.statistics().compiled_rules, 6u);
  auto second = analysis.queryFrom(1);
  EXPECT_EQ(second.statistics().slice_cache_hits, 1u);
  EXPECT_EQ(second.statistics().compiled_rules, 0u);
  EXPECT_EQ(second.statistics().prepared_weights, 0u);
  (void)first.compare(0);
  (void)first.compare(1);
  const auto products = first.statistics().algebra.matrix_products;
  (void)first.compare(2);
  EXPECT_EQ(first.statistics().algebra.matrix_products, products);
  const auto before_second = second.statistics().algebra.matrix_products;
  first.prepareReadout();
  EXPECT_EQ(second.statistics().algebra.matrix_products, before_second);
  EXPECT_LT(analysis.analyzeAll().statistics.saturation.rules,
            graph.vertices().size() * graph.edges().size() * 2);
  Options uncached;
  uncached.max_cached_slices = 0;
  EXPECT_EQ(analysis.analyzeAll().pairs,
            Solver(uncached).prepare(graph, observer).analyzeAll().pairs);
}

TEST(InterleavedDyckAffineSPDS, DemandCorridorAndObserverAllocationLimit) {
  Graph graph;
  graph.addEdge(0, 1, Label::neutral());
  graph.addEdge(1, 2, Label::neutral());
  for (Vertex v = 10; v < 100; ++v)
    graph.addEdge(v == 10 ? 0 : v - 1, v, Label::neutral());
  const auto observer = HistoryObserver::parity({graph.edges()[0]});
  const auto analysis = Solver{}.prepare(graph, observer);
  const auto demands = analysis.analyzeDemands({{0, 2}}, ComparisonMode::Joint,
                                               spds::DemandDirection::Post);
  EXPECT_TRUE(demands.mayReach(0, 2));
  EXPECT_EQ(demands.statistics.compiled_rules, 4u);
  Options options;
  options.max_matrix_dimension = 2;
  options.observer.max_events = std::numeric_limits<std::size_t>::max();
  options.observer.max_order_pairs = std::numeric_limits<std::size_t>::max();
  EXPECT_THROW(Solver(options).prepare(graph), spds::ResourceLimit);
  const auto identity =
      Solver{}.prepare(graph, HistoryObserver(32)).analyzeAll();
  EXPECT_EQ(identity.statistics.prepared_weights, 0u);
  EXPECT_EQ(identity.statistics.compiled_rules, 0u);
}
} // namespace
