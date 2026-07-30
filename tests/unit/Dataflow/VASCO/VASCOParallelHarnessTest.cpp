#include "Dataflow/VASCO/VASCO.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/Support/CommandLine.h>

namespace {

using Method = std::string;
using Node = std::string;

class ToyGraph final : public vasco::DirectedGraph<Node> {
public:
  ToyGraph(std::vector<Node> Nodes, std::vector<Node> Heads,
           std::vector<Node> Tails, std::vector<std::pair<Node, Node>> Edges)
      : Nodes(std::move(Nodes)), Heads(std::move(Heads)),
        Tails(std::move(Tails)) {
    for (const auto &Edge : Edges) {
      Succs[Edge.first].push_back(Edge.second);
      Preds[Edge.second].push_back(Edge.first);
    }
  }

  std::vector<Node> nodes() const override { return Nodes; }
  std::vector<Node> heads() const override { return Heads; }
  std::vector<Node> tails() const override { return Tails; }

  std::vector<Node> predsOf(const Node &N) const override {
    auto It = Preds.find(N);
    return It == Preds.end() ? std::vector<Node>{} : It->second;
  }

  std::vector<Node> succsOf(const Node &N) const override {
    auto It = Succs.find(N);
    return It == Succs.end() ? std::vector<Node>{} : It->second;
  }

  std::size_t size() const override { return Nodes.size(); }

private:
  std::vector<Node> Nodes;
  std::vector<Node> Heads;
  std::vector<Node> Tails;
  std::map<Node, std::vector<Node>> Preds;
  std::map<Node, std::vector<Node>> Succs;
};

class ToyProgramRepresentation final
    : public vasco::ProgramRepresentation<Method, Node> {
public:
  using GraphPtr = std::shared_ptr<const vasco::DirectedGraph<Node>>;

  void addMethod(const Method &MethodName, GraphPtr Graph) {
    Graphs[MethodName] = std::move(Graph);
  }

  void addEntryPoint(const Method &MethodName) {
    EntryPoints.push_back(MethodName);
  }

  void addCall(const Node &CallNode, std::vector<Method> Targets) {
    Calls[CallNode] = std::move(Targets);
  }

  std::vector<Method> getEntryPoints() const override { return EntryPoints; }

  GraphPtr getControlFlowGraph(const Method &MethodName) const override {
    return Graphs.at(MethodName);
  }

  bool isCall(const Node &NodeName) const override {
    return Calls.find(NodeName) != Calls.end();
  }

  std::optional<std::vector<Method>>
  resolveTargets(const Method &, const Node &CallNode) const override {
    auto It = Calls.find(CallNode);
    if (It == Calls.end()) {
      return std::vector<Method>{};
    }
    return It->second;
  }

private:
  std::vector<Method> EntryPoints;
  std::map<Method, GraphPtr> Graphs;
  std::map<Node, std::vector<Method>> Calls;
};

enum class Sign {
  Top,
  Negative,
  Zero,
  Positive,
  Bottom,
};

Sign meetSign(Sign LHS, Sign RHS) {
  if (LHS == RHS) {
    return LHS;
  }
  if (LHS == Sign::Top) {
    return RHS;
  }
  if (RHS == Sign::Top) {
    return LHS;
  }
  return Sign::Bottom;
}

class ForwardSignAnalysis final
    : public vasco::ForwardInterProceduralAnalysis<Method, Node, Sign> {
public:
  explicit ForwardSignAnalysis(const ToyProgramRepresentation &Program)
      : Program(Program) {}

  Sign boundaryValue(const Method &) override { return Sign::Top; }
  Sign copy(const Sign &Src) override { return Src; }
  Sign meet(const Sign &LHS, const Sign &RHS) override {
    return meetSign(LHS, RHS);
  }
  const vasco::ProgramRepresentation<Method, Node> &
  programRepresentation() const override {
    return Program;
  }
  Sign topValue() override { return Sign::Top; }

protected:
  Sign normalFlowFunction(ContextPtr, const Node &NodeName,
                          const Sign &InValue) override {
    if (NodeName.find(".assign") != std::string::npos) {
      return Sign::Positive;
    }
    return InValue;
  }

  Sign callEntryFlowFunction(ContextPtr, const Method &, const Node &,
                             const Sign &InValue) override {
    return InValue;
  }

  Sign callExitFlowFunction(ContextPtr, const Method &, const Node &,
                            const Sign &ExitValue) override {
    return ExitValue;
  }

  Sign callLocalFlowFunction(ContextPtr, const Node &,
                             const Sign &InValue) override {
    return InValue;
  }

private:
  const ToyProgramRepresentation &Program;
};

class MutualRecursiveAnalysis final
    : public vasco::ForwardInterProceduralAnalysis<Method, Node, Sign> {
public:
  explicit MutualRecursiveAnalysis(const ToyProgramRepresentation &Program)
      : Program(Program) {}

  Sign boundaryValue(const Method &) override { return Sign::Top; }
  Sign copy(const Sign &Src) override { return Src; }
  Sign meet(const Sign &LHS, const Sign &RHS) override {
    return meetSign(LHS, RHS);
  }
  const vasco::ProgramRepresentation<Method, Node> &
  programRepresentation() const override {
    return Program;
  }
  Sign topValue() override { return Sign::Top; }

protected:
  Sign normalFlowFunction(ContextPtr, const Node &,
                          const Sign &InValue) override {
    return InValue;
  }

  Sign callEntryFlowFunction(ContextPtr, const Method &, const Node &,
                             const Sign &InValue) override {
    return InValue;
  }

  Sign callExitFlowFunction(ContextPtr, const Method &, const Node &,
                            const Sign &ExitValue) override {
    return ExitValue;
  }

  Sign callLocalFlowFunction(ContextPtr, const Node &,
                             const Sign &InValue) override {
    const unsigned Arrival = BarrierArrivals.fetch_add(1) + 1;
    if (Arrival <= 2) {
      std::unique_lock<std::mutex> Lock(BarrierMutex);
      if (Arrival == 2) {
        BarrierCondition.notify_all();
      } else {
        BarrierCondition.wait(Lock,
                              [this] { return BarrierArrivals.load() >= 2; });
      }
    }
    return InValue;
  }

private:
  const ToyProgramRepresentation &Program;
  std::atomic<unsigned> BarrierArrivals{0};
  std::mutex BarrierMutex;
  std::condition_variable BarrierCondition;
};

using FactSet = std::set<std::string>;

class BackwardIdentityAnalysis final
    : public vasco::BackwardInterProceduralAnalysis<Method, Node, FactSet> {
public:
  explicit BackwardIdentityAnalysis(const ToyProgramRepresentation &Program)
      : Program(Program) {}

  FactSet boundaryValue(const Method &) override { return {"ret"}; }
  FactSet copy(const FactSet &Src) override { return Src; }
  FactSet meet(const FactSet &LHS, const FactSet &RHS) override {
    FactSet Result = LHS;
    Result.insert(RHS.begin(), RHS.end());
    return Result;
  }
  const vasco::ProgramRepresentation<Method, Node> &
  programRepresentation() const override {
    return Program;
  }
  FactSet topValue() override { return {}; }

protected:
  FactSet normalFlowFunction(ContextPtr, const Node &,
                             const FactSet &OutValue) override {
    return OutValue;
  }

  FactSet callEntryFlowFunction(ContextPtr, const Method &, const Node &,
                                const FactSet &EntryValue) override {
    return EntryValue;
  }

  FactSet callExitFlowFunction(ContextPtr, const Method &, const Node &,
                               const FactSet &OutValue) override {
    return OutValue;
  }

  FactSet callLocalFlowFunction(ContextPtr, const Node &,
                                const FactSet &OutValue) override {
    return OutValue;
  }

private:
  const ToyProgramRepresentation &Program;
};

ToyProgramRepresentation buildTwoEntryForwardProgram() {
  ToyProgramRepresentation Program;
  for (const Method &Entry : {"main1", "main2"}) {
    Program.addMethod(Entry,
                      std::make_shared<ToyGraph>(
                          std::vector<Node>{Entry + ".assign", Entry + ".call",
                                            Entry + ".exit"},
                          std::vector<Node>{Entry + ".assign"},
                          std::vector<Node>{Entry + ".exit"},
                          std::vector<std::pair<Node, Node>>{
                              {Entry + ".assign", Entry + ".call"},
                              {Entry + ".call", Entry + ".exit"}}));
    Program.addEntryPoint(Entry);
    Program.addCall(Entry + ".call", {"callee"});
  }
  Program.addMethod("callee", std::make_shared<ToyGraph>(
                                  std::vector<Node>{"callee.exit"},
                                  std::vector<Node>{"callee.exit"},
                                  std::vector<Node>{"callee.exit"},
                                  std::vector<std::pair<Node, Node>>{}));
  return Program;
}

ToyProgramRepresentation buildBackwardProgram() {
  ToyProgramRepresentation Program;
  Program.addMethod(
      "main",
      std::make_shared<ToyGraph>(
          std::vector<Node>{"main.call", "main.return"},
          std::vector<Node>{"main.call"}, std::vector<Node>{"main.return"},
          std::vector<std::pair<Node, Node>>{{"main.call", "main.return"}}));
  Program.addMethod("callee", std::make_shared<ToyGraph>(
                                  std::vector<Node>{"callee.return"},
                                  std::vector<Node>{"callee.return"},
                                  std::vector<Node>{"callee.return"},
                                  std::vector<std::pair<Node, Node>>{}));
  Program.addEntryPoint("main");
  Program.addCall("main.call", {"callee"});
  return Program;
}

ToyProgramRepresentation buildMutuallyRecursiveProgram() {
  ToyProgramRepresentation Program;
  for (const Method &MethodName : {"x", "y"}) {
    const Node CallNode = MethodName + ".call";
    Program.addMethod(
        MethodName,
        std::make_shared<ToyGraph>(
            std::vector<Node>{CallNode}, std::vector<Node>{CallNode},
            std::vector<Node>{CallNode},
            std::vector<std::pair<Node, Node>>{}));
    Program.addEntryPoint(MethodName);
  }
  Program.addCall("x.call", {"y"});
  Program.addCall("y.call", {"x"});
  return Program;
}

TEST(VASCOParallelHarnessTest, ParallelForwardMatchesSequentialAndInterns) {
  ToyProgramRepresentation Program = buildTwoEntryForwardProgram();

  ForwardSignAnalysis Serial(Program);
  Serial.doAnalysis();

  ForwardSignAnalysis Parallel(Program);
  Parallel.setParallelContextScheduling(true);
  Parallel.setContextStepBudget(1);
  Parallel.doAnalysis();

  ASSERT_EQ(Parallel.getContexts("callee").size(), 1U);
  EXPECT_EQ(Parallel.getContexts("callee").front()->getEntryValue(),
            Sign::Positive);
  EXPECT_EQ(Parallel.getContexts("callee").front()->getExitValue(),
            Sign::Positive);
  EXPECT_GT(Parallel.getContexts("callee").front()->getSummaryVersion(), 0U);
  EXPECT_EQ(
      Parallel.getMeetOverValidPathsSolution().getValueAfter("main1.exit"),
      Serial.getMeetOverValidPathsSolution().getValueAfter("main1.exit"));
  EXPECT_EQ(
      Parallel.getMeetOverValidPathsSolution().getValueAfter("main2.exit"),
      Serial.getMeetOverValidPathsSolution().getValueAfter("main2.exit"));

  if (ThreadPool::get()->hasWorkers()) {
    EXPECT_GT(Parallel.getSchedulerStats().parallel_worker_tasks, 0U);
    EXPECT_GT(Parallel.getSchedulerStats().context_batches, 0U);
    EXPECT_GT(Parallel.getSchedulerStats().contexts_reused, 0U);
    EXPECT_GT(Parallel.getSchedulerStats().summary_publications, 0U);
    EXPECT_GT(Parallel.getSchedulerStats().stale_callsite_replays, 0U);
  }
}

TEST(VASCOParallelHarnessTest, ParallelBackwardMatchesSequential) {
  ToyProgramRepresentation Program = buildBackwardProgram();

  BackwardIdentityAnalysis Serial(Program);
  Serial.doAnalysis();

  BackwardIdentityAnalysis Parallel(Program);
  Parallel.setParallelContextScheduling(true);
  Parallel.setContextStepBudget(1);
  Parallel.doAnalysis();

  ASSERT_EQ(Parallel.getContexts("main").size(), 1U);
  EXPECT_EQ(Parallel.getContexts("main").front()->getEntryValue(),
            Serial.getContexts("main").front()->getEntryValue());
  ASSERT_EQ(Parallel.getContexts("callee").size(), 1U);
  EXPECT_EQ(Parallel.getContexts("callee").front()->getEntryValue(),
            Serial.getContexts("callee").front()->getEntryValue());
  EXPECT_GT(Parallel.getContexts("callee").front()->getSummaryVersion(), 0U);
  if (ThreadPool::get()->hasWorkers()) {
    EXPECT_GT(Parallel.getSchedulerStats().stale_callsite_replays, 0U);
  }
}

TEST(VASCOParallelHarnessTest, MutualRecursionPublishesWithoutContextLockCycle) {
  if (!ThreadPool::get()->hasWorkers()) {
    GTEST_SKIP() << "requires parallel VASCO workers";
  }

  ToyProgramRepresentation Program = buildMutuallyRecursiveProgram();
  MutualRecursiveAnalysis Analysis(Program);
  Analysis.setParallelContextScheduling(true);
  Analysis.setContextStepBudget(2);
  Analysis.doAnalysis();

  ASSERT_EQ(Analysis.getContexts("x").size(), 1U);
  ASSERT_EQ(Analysis.getContexts("y").size(), 1U);
  EXPECT_GT(Analysis.getContexts("x").front()->getSummaryVersion(), 0U);
  EXPECT_GT(Analysis.getContexts("y").front()->getSummaryVersion(), 0U);
  EXPECT_GT(Analysis.getSchedulerStats().stale_callsite_replays, 0U);
}

TEST(VASCOParallelHarnessTest, SummarySnapshotKeepsValueAndVersionCoherent) {
  struct CheckedValue {
    std::size_t Sequence = 0;
    std::string Checksum;
  };

  using CheckedContext = vasco::Context<Method, Node, CheckedValue>;
  auto Context = std::make_shared<CheckedContext>("summary-owner");
  std::atomic<bool> WriterDone{false};
  std::atomic<bool> Coherent{true};

  std::thread Writer([&] {
    for (std::size_t Sequence = 1; Sequence <= 2000; ++Sequence) {
      const CheckedValue Value{Sequence, std::to_string(Sequence)};
      std::lock_guard<std::recursive_mutex> Lock(Context->mutex());
      Context->setExitValue(Value);
      Context->publishSummary(Value);
    }
    WriterDone.store(true);
  });

  std::vector<std::thread> Readers;
  for (unsigned I = 0; I < 4; ++I) {
    Readers.emplace_back([&] {
      while (!WriterDone.load()) {
        const auto Snapshot = Context->getSummarySnapshot();
        if (Snapshot &&
            Snapshot->Value.Checksum !=
                std::to_string(Snapshot->Value.Sequence)) {
          Coherent.store(false);
        }
      }
    });
  }

  Writer.join();
  for (auto &Reader : Readers) {
    Reader.join();
  }

  const auto Snapshot = Context->getSummarySnapshot();
  ASSERT_TRUE(Snapshot);
  EXPECT_TRUE(Coherent.load());
  EXPECT_EQ(Snapshot->Value.Sequence, 2000U);
  EXPECT_EQ(Snapshot->Value.Checksum, "2000");
  EXPECT_EQ(Snapshot->Version, 2000U);
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "VASCO parallel scheduler harness\n");
  return RUN_ALL_TESTS();
}
