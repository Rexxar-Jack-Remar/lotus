#include "Alias/InclusionBased/GPG/GPU.h"

#include <vector>

#include <gtest/gtest.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

using namespace lotus::gpg;

namespace {

Access access(LocationId location, IndirectionList indirections,
              bool k_limited = false, const llvm::Type *type = nullptr,
              bool upward_exposed = false) {
  Access result;
  result.location = location;
  result.indirections = std::move(indirections);
  result.upward_exposed = upward_exposed;
  result.type = type;
  result.k_limited = k_limited;
  return result;
}

Access dereferenceAccess(LocationId location, unsigned count,
                         bool k_limited = false,
                         const llvm::Type *type = nullptr) {
  return access(location, IndirectionList::dereferences(count), k_limited,
                type);
}

GPU gpu(Access source, Access target, StatementId statement) {
  GPU result;
  result.source = std::move(source);
  result.target = std::move(target);
  result.statement = statement;
  return result;
}

} // namespace

TEST(GPGAlgebraRegression, SummarizedRemainderCoversAppendixB8EqualCase) {
  const Indirection dereference = Indirection::dereference();
  const Indirection field_n = Indirection::fieldAt(7);
  const Indirection any_field = Indirection::anyField();
  const IndirectionList summarized({dereference, field_n, field_n}, true);

  const std::vector<IndirectionList> remainders = summarized.remaindersAfter(
      IndirectionList({dereference, field_n, field_n}), 3);

  ASSERT_EQ(remainders.size(), 4u);
  EXPECT_EQ(remainders[0], IndirectionList());
  EXPECT_EQ(remainders[1], IndirectionList({any_field}));
  EXPECT_EQ(remainders[2], IndirectionList({any_field, any_field}));
  EXPECT_EQ(remainders[3],
            IndirectionList({any_field, any_field, any_field}, true));
}

TEST(GPGAlgebraRegression, RemovingEmptyPrefixPreservesSummary) {
  const IndirectionList summarized(
      {Indirection::dereference(), Indirection::fieldAt(7)}, true);

  const std::vector<IndirectionList> remainders =
      summarized.remaindersAfter(IndirectionList());

  ASSERT_EQ(remainders.size(), 1u);
  EXPECT_EQ(remainders.front(), summarized);
}

TEST(GPGAlgebraRegression, DesirabilityComparesStoredLengthsOnly) {
  const IndirectionList exact = IndirectionList::dereferences(3);
  const IndirectionList summarized({Indirection::dereference(),
                                    Indirection::dereference(),
                                    Indirection::dereference()},
                                   true);

  EXPECT_TRUE(exact.doesNotExceed(summarized));
  EXPECT_TRUE(summarized.doesNotExceed(exact));
}

TEST(GPGAlgebraRegression, NonFieldOrderingAgreesWithEquality) {
  const Indirection left{IndirectionKind::Dereference, 17};
  const Indirection right{IndirectionKind::Dereference, 29};

  EXPECT_EQ(left, right);
  EXPECT_FALSE(left < right);
  EXPECT_FALSE(right < left);
}

TEST(GPGAlgebraRegression, TargetAndSourceCompositionsAreBothApplied) {
  const GPU consumer = gpu(dereferenceAccess(1, 2), dereferenceAccess(1, 2), 2);
  const GPU producer = gpu(dereferenceAccess(1, 1), dereferenceAccess(2, 0), 1);

  const ReductionResult reduced =
      reduceGPU(consumer, GPUSet({producer}), GPUSet({producer}));

  ASSERT_EQ(reduced.reduced.size(), 1u);
  EXPECT_EQ(reduced.reduced.begin()->source, dereferenceAccess(2, 1));
  EXPECT_EQ(reduced.reduced.begin()->target, dereferenceAccess(2, 1));
}

TEST(GPGAlgebraRegression, UsedStopsHeapCycleAfterOneCompositionPerChain) {
  const Indirection star = Indirection::dereference();
  const Indirection field_n = Indirection::fieldAt(7);
  const Indirection any_field = Indirection::anyField();

  const GPU consumer =
      gpu(access(1, IndirectionList({star}), true),
          access(1, IndirectionList({star, field_n, field_n}, true), true), 11);
  const GPU to_first_heap = gpu(access(1, IndirectionList({star}), true),
                                access(2, IndirectionList(), true), 15);
  const GPU to_recursive_heap = gpu(access(2, IndirectionList({field_n}), true),
                                    access(3, IndirectionList(), true), 17);
  const GPU heap_self_cycle = gpu(access(3, IndirectionList({field_n}), true),
                                  access(3, IndirectionList(), true), 18);
  const GPUSet context = {to_first_heap, to_recursive_heap, heap_self_cycle};

  const ReductionResult result = reduceGPU(consumer, context, context, 3);

  GPUSet expected;
  expected.insert(gpu(access(1, IndirectionList({star}), true),
                      access(3, IndirectionList(), true), 11));
  expected.insert(gpu(access(1, IndirectionList({star}), true),
                      access(3, IndirectionList({any_field}), true), 11));
  expected.insert(gpu(access(1, IndirectionList({star}), true),
                      access(3, IndirectionList({any_field, any_field}), true),
                      11));
  expected.insert(gpu(
      access(1, IndirectionList({star}), true),
      access(3, IndirectionList({any_field, any_field, any_field}, true), true),
      11));
  EXPECT_EQ(result.reduced, expected);
  EXPECT_TRUE(result.queued.empty());
}

TEST(GPGAlgebraRegression, QueuesUndesirableHalfOfDualComposition) {
  const IndirectionList summarized({Indirection::dereference(),
                                    Indirection::dereference(),
                                    Indirection::dereference()},
                                   true);
  const GPU consumer =
      gpu(dereferenceAccess(1, 2), access(1, summarized, true), 2);
  const GPU producer = gpu(dereferenceAccess(1, 1), dereferenceAccess(2, 2), 1);

  const GPUSet context = {producer};
  const ReductionResult result = reduceGPU(consumer, context, context, 3);

  ASSERT_EQ(result.reduced.size(), 1u);
  EXPECT_EQ(result.reduced.begin()->source, dereferenceAccess(1, 2));
  EXPECT_EQ(result.reduced.begin()->target,
            access(2, IndirectionList({Indirection::dereference(),
                                       Indirection::dereference(),
                                       Indirection::dereference()},
                                      true)));
  EXPECT_EQ(result.queued, context);
}

TEST(GPGAlgebraRegression, QueuesBlockedProducerReachedAfterComposition) {
  const GPU consumer = gpu(dereferenceAccess(1, 1), dereferenceAccess(2, 1), 3);
  const GPU first = gpu(dereferenceAccess(2, 1), dereferenceAccess(3, 1), 1);
  const GPU blocked = gpu(dereferenceAccess(3, 1), dereferenceAccess(4, 0), 2);

  const ReductionResult result =
      reduceGPU(consumer, GPUSet({first}), GPUSet({first, blocked}));

  EXPECT_EQ(result.reduced,
            GPUSet({gpu(dereferenceAccess(1, 1), dereferenceAccess(3, 1), 3)}));
  EXPECT_EQ(result.queued, GPUSet({blocked}));
}

TEST(GPGAlgebraRegression, ProducerIndexDeduplicatesSemanticProducers) {
  const GPU consumer = gpu(dereferenceAccess(1, 1), dereferenceAccess(2, 1), 2);
  const GPU producer = gpu(dereferenceAccess(2, 1), dereferenceAccess(3, 0), 1);
  const GPUSet first = {producer};
  const GPUSet second = {producer};
  GPUProducerIndex index;
  index.add(first);
  index.add(second);

  EXPECT_EQ(index.candidates(consumer).size(), 1u);
}

TEST(GPGAlgebraRegression, DefiniteDependenceMatchesPaperEquations) {
  const GPU producer = gpu(dereferenceAccess(1, 2), dereferenceAccess(2, 1), 1);

  EXPECT_EQ(
      definiteDependence(
          gpu(dereferenceAccess(1, 2), dereferenceAccess(3, 0), 2), producer),
      Dependence::WriteAfterWrite);
  EXPECT_TRUE(hasDependence(
      definiteDependence(
          gpu(dereferenceAccess(1, 1), dereferenceAccess(3, 0), 2), producer),
      Dependence::WriteAfterRead));
  EXPECT_TRUE(hasDependence(
      definiteDependence(
          gpu(dereferenceAccess(2, 1), dereferenceAccess(3, 0), 2), producer),
      Dependence::WriteAfterRead));
  EXPECT_TRUE(hasDependence(
      definiteDependence(
          gpu(dereferenceAccess(1, 3), dereferenceAccess(3, 0), 2), producer),
      Dependence::ReadAfterWrite));
  EXPECT_TRUE(hasDependence(
      definiteDependence(
          gpu(dereferenceAccess(3, 1), dereferenceAccess(1, 2), 2), producer),
      Dependence::ReadAfterWrite));
}

TEST(GPGAlgebraRegression, DisjointDefinitionDoesNotHideTargetDependence) {
  llvm::LLVMContext context;
  const llvm::Type *pointer_type = llvm::Type::getInt8PtrTy(context);
  const llvm::Type *other_type = llvm::Type::getInt32PtrTy(context);
  const Indirection star = Indirection::dereference();

  const GPU producer =
      gpu(access(1, IndirectionList({star, Indirection::fieldAt(1)}), false,
                 pointer_type),
          dereferenceAccess(4, 0), 1);
  GPU consumer =
      gpu(access(1, IndirectionList({star, Indirection::fieldAt(2)}), false,
                 pointer_type),
          access(3, IndirectionList({star}), false, pointer_type), 2);
  const TypeCompatibility same_type =
      [](const llvm::Type *lhs, const llvm::Type *rhs) { return lhs == rhs; };

  EXPECT_TRUE(potentialDependence(consumer, producer, same_type));
  consumer.target.type = other_type;
  EXPECT_FALSE(potentialDependence(consumer, producer, same_type));
}

TEST(GPGAlgebraRegression, PotentialDependenceUsesProducerDefinition) {
  llvm::LLVMContext context;
  const llvm::Type *producer_definition = llvm::Type::getInt8PtrTy(context);
  const llvm::Type *producer_reference = llvm::Type::getInt16PtrTy(context);
  const GPU producer =
      gpu(dereferenceAccess(1, 2, false, producer_definition),
          dereferenceAccess(2, 1, false, producer_reference), 1);
  const GPU consumer =
      gpu(dereferenceAccess(3, 2, false, producer_reference),
          dereferenceAccess(4, 1, false, producer_reference), 2);
  const TypeCompatibility same_type =
      [](const llvm::Type *lhs, const llvm::Type *rhs) { return lhs == rhs; };

  // Only the earlier GPU's reference has a matching type.  That is the
  // reverse (WaR) direction, not a potential dependence of consumer on
  // producer.
  EXPECT_FALSE(potentialDependence(consumer, producer, same_type));
}
