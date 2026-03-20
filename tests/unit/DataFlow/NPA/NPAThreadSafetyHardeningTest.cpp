#include "Dataflow/NPA/Domains/BitVectorDomain.h"
#include "Dataflow/NPA/Domains/GenKillDomain.h"
#include "Dataflow/NPA/Domains/TaintTransferDomain.h"

#include <algorithm>
#include <thread>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <gtest/gtest.h>

namespace {

template <typename Fn> std::vector<int> runOnThreads(unsigned count, Fn fn) {
  std::vector<int> results(count, 0);
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    threads.emplace_back([&, i] { results[i] = fn() ? 1 : 0; });
  }
  for (auto &thread : threads)
    thread.join();
  return results;
}

} // namespace

TEST(NPAThreadSafetyHardening, ConfiguredDomainWidthsAreVisibleAcrossThreads) {
  npa::TaintTransferDomain::setBitWidth(7);
  npa::BitVectorDomain::setBitWidth(9);
  npa::GenKillTransferDomain::setBitWidth(11);

  auto results = runOnThreads(4, [] {
    auto taintZero = npa::TaintTransferDomain::zero();
    auto taintOne = npa::TaintTransferDomain::one();
    auto bitsZero = npa::BitVectorDomain::zero();
    auto bitsOne = npa::BitVectorDomain::one();
    auto genKillZero = npa::GenKillTransferDomain::zero();
    auto genKillOne = npa::GenKillTransferDomain::one();

    return taintZero.gen.getBitWidth() == 7 &&
           taintOne.gen.getBitWidth() == 7 && taintOne.rel.size() == 7 &&
           taintOne.rel.front().getBitWidth() == 7 &&
           bitsZero.getBitWidth() == 9 && bitsOne.getBitWidth() == 9 &&
           genKillZero.first.getBitWidth() == 11 &&
           genKillZero.second.getBitWidth() == 11 &&
           genKillOne.first.getBitWidth() == 11 &&
           genKillOne.second.getBitWidth() == 11;
  });

  EXPECT_TRUE(
      std::all_of(results.begin(), results.end(), [](int ok) { return ok; }));
}

TEST(NPAThreadSafetyHardening, SafeCoreDomainsSupportConcurrentReadOnlyOps) {
  npa::TaintTransferDomain::setBitWidth(4);
  npa::BitVectorDomain::setBitWidth(4);
  npa::GenKillTransferDomain::setBitWidth(4);

  auto transfer = npa::TaintTransferDomain::one();
  npa::TaintTransferDomain::addEdge(transfer, 0, 1);
  npa::TaintTransferDomain::addEdge(transfer, 1, 2);
  npa::TaintTransferDomain::addGen(transfer, 3);
  const auto composed = npa::TaintTransferDomain::extend(transfer, transfer);
  llvm::APInt input(4, 0);
  input.setBit(0);
  const llvm::APInt expectedTaint = npa::TaintTransferDomain::apply(composed, input);

  llvm::APInt bitsA(4, 0);
  bitsA.setBit(0);
  bitsA.setBit(2);
  llvm::APInt bitsB(4, 0);
  bitsB.setBit(1);
  bitsB.setBit(2);
  const llvm::APInt expectedBitVector =
      npa::BitVectorDomain::combine(bitsA, bitsB);

  npa::GenKillTransferDomain::value_type genKillA{
      llvm::APInt(4, 0b0011), llvm::APInt(4, 0b0100)};
  npa::GenKillTransferDomain::value_type genKillB{
      llvm::APInt(4, 0b1000), llvm::APInt(4, 0b0001)};
  const auto expectedGenKill = npa::GenKillTransferDomain::extend(genKillA, genKillB);

  auto results = runOnThreads(4, [&] {
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
      if (npa::TaintTransferDomain::apply(composed, input) != expectedTaint)
        return false;
      if (!npa::TaintTransferDomain::equal(
              npa::TaintTransferDomain::extend(transfer, transfer), composed)) {
        return false;
      }
      if (npa::BitVectorDomain::combine(bitsA, bitsB) != expectedBitVector)
        return false;
      if (npa::GenKillTransferDomain::extend(genKillA, genKillB) !=
          expectedGenKill) {
        return false;
      }
    }
    return true;
  });

  EXPECT_TRUE(
      std::all_of(results.begin(), results.end(), [](int ok) { return ok; }));
}
