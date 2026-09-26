#include "Dataflow/APA/Solver/Inter/CallGraph.h"

#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Minimal configurable interprocedural-CFG stub for call-graph tests.
// Procedures and instructions are ints. A procedure is "opaque" (external) iff
// it has no start or exit points.
struct FakeCG {
  std::unordered_map<int, std::vector<int>> Insts;    // proc -> instructions
  std::unordered_map<int, std::vector<int>> Callees;  // call inst -> callees
  std::unordered_map<int, std::vector<int>> Starts;   // proc -> start points
  std::unordered_map<int, std::vector<int>> Exits;    // proc -> exit points

  std::vector<int> getAllInstructionsOf(int F) const {
    auto It = Insts.find(F);
    return It == Insts.end() ? std::vector<int>{} : It->second;
  }
  bool isCallSite(int Inst) const { return Callees.count(Inst) != 0; }
  std::vector<int> getCalleesOfCallAt(int Inst) const {
    auto It = Callees.find(Inst);
    return It == Callees.end() ? std::vector<int>{} : It->second;
  }
  std::vector<int> getStartPointsOf(int F) const {
    auto It = Starts.find(F);
    return It == Starts.end() ? std::vector<int>{} : It->second;
  }
  std::vector<int> getExitPointsOf(int F) const {
    auto It = Exits.find(F);
    return It == Exits.end() ? std::vector<int>{} : It->second;
  }

  // Helper: declare a normal (non-opaque) procedure with one call-site inst
  // pointing at the given callees.
  void addProc(int F, std::vector<int> CalleeProcs) {
    Starts[F] = {F * 100};      // arbitrary distinct entry
    Exits[F] = {F * 100 + 1};   // arbitrary distinct exit
    const int CallInst = F * 100 + 2;
    Insts[F] = {F * 100, CallInst, F * 100 + 1};
    if (!CalleeProcs.empty()) {
      Callees[CallInst] = std::move(CalleeProcs);
    }
  }
};

using Builder = elimination::CallGraphSCCBuilder<int, int, FakeCG>;
using Result = elimination::CallGraphSCCResult<int>;

// Index of the component containing proc F (or -1).
int compOf(const Result &R, int F) {
  for (std::size_t I = 0; I < R.order.size(); ++I) {
    for (int P : R.order[I].procs) {
      if (P == F) {
        return static_cast<int>(I);
      }
    }
  }
  return -1;
}

TEST(CallGraphSCC, LinearChainReverseTopo) {
  FakeCG CG;
  CG.addProc(1, {2}); // A -> B
  CG.addProc(2, {3}); // B -> C
  CG.addProc(3, {});  // C -> (leaf)

  Builder B(CG);
  auto R = B.build({1});

  ASSERT_EQ(R.order.size(), 3u);
  // Callees before callers: C < B < A.
  EXPECT_LT(compOf(R, 3), compOf(R, 2));
  EXPECT_LT(compOf(R, 2), compOf(R, 1));
  for (const auto &C : R.order) {
    EXPECT_FALSE(C.recursive);
    EXPECT_EQ(C.procs.size(), 1u);
  }
}

TEST(CallGraphSCC, DirectSelfRecursion) {
  FakeCG CG;
  CG.addProc(1, {1}); // A -> A

  Builder B(CG);
  auto R = B.build({1});

  ASSERT_EQ(R.order.size(), 1u);
  EXPECT_EQ(R.order[0].procs.size(), 1u);
  EXPECT_TRUE(R.order[0].recursive);
}

TEST(CallGraphSCC, MutualRecursionFormsOneComponent) {
  FakeCG CG;
  CG.addProc(1, {2}); // A -> B
  CG.addProc(2, {1}); // B -> A

  Builder B(CG);
  auto R = B.build({1});

  ASSERT_EQ(R.order.size(), 1u);
  EXPECT_EQ(R.order[0].procs.size(), 2u);
  EXPECT_TRUE(R.order[0].recursive);
  EXPECT_EQ(compOf(R, 1), compOf(R, 2));
}

TEST(CallGraphSCC, DiamondOrdersLeafFirstRootLast) {
  FakeCG CG;
  // A -> B, A -> C ; B -> D ; C -> D.
  CG.Starts[1] = {100};
  CG.Exits[1] = {101};
  CG.Insts[1] = {100, 102, 103, 101};
  CG.Callees[102] = {2};
  CG.Callees[103] = {3};
  CG.addProc(2, {4}); // B -> D
  CG.addProc(3, {4}); // C -> D
  CG.addProc(4, {});  // D leaf

  Builder B(CG);
  auto R = B.build({1});

  ASSERT_EQ(R.order.size(), 4u);
  // D before B and C; B and C before A.
  EXPECT_LT(compOf(R, 4), compOf(R, 2));
  EXPECT_LT(compOf(R, 4), compOf(R, 3));
  EXPECT_LT(compOf(R, 2), compOf(R, 1));
  EXPECT_LT(compOf(R, 3), compOf(R, 1));
}

TEST(CallGraphSCC, ExternalCalleeExcluded) {
  FakeCG CG;
  CG.addProc(1, {9}); // A -> ext(9)
  // Proc 9 has no start/exit points -> opaque, not a node.

  Builder B(CG);
  auto R = B.build({1});

  ASSERT_EQ(R.order.size(), 1u);
  EXPECT_EQ(R.order[0].procs.size(), 1u);
  EXPECT_EQ(R.order[0].procs[0], 1);
  EXPECT_EQ(compOf(R, 9), -1); // external callee never interned
}

TEST(CallGraphSCC, OpaqueEntryYieldsEmptyGraph) {
  FakeCG CG; // proc 1 declared with no start/exit -> opaque

  Builder B(CG);
  auto R = B.build({1});

  EXPECT_TRUE(R.order.empty());
}

} // namespace
