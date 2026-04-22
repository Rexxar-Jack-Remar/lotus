#include "Dataflow/VASCO/VASCO.h"

#include <gtest/gtest.h>

#include <sstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using Method = std::string;
using Node = std::string;

class ToyGraph final : public vasco::DirectedGraph<Node> {
public:
  ToyGraph(std::vector<Node> Nodes, std::vector<Node> Heads,
           std::vector<Node> Tails,
           std::vector<std::pair<Node, Node>> Edges)
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
    if (It == Preds.end()) {
      return {};
    }
    return It->second;
  }

  std::vector<Node> succsOf(const Node &N) const override {
    auto It = Succs.find(N);
    if (It == Succs.end()) {
      return {};
    }
    return It->second;
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

  void addEntryPoint(const Method &MethodName) { EntryPoints.push_back(MethodName); }

  void addCall(const Node &CallNode, std::vector<Method> Targets) {
    Calls[CallNode] = std::move(Targets);
  }

  void addUnknownCall(const Node &CallNode) { UnknownCalls.insert(CallNode); }

  std::vector<Method> getEntryPoints() const override { return EntryPoints; }

  GraphPtr getControlFlowGraph(const Method &MethodName) const override {
    return Graphs.at(MethodName);
  }

  bool isCall(const Node &NodeName) const override {
    return Calls.find(NodeName) != Calls.end() || UnknownCalls.count(NodeName);
  }

  std::optional<std::vector<Method>>
  resolveTargets(const Method &, const Node &CallNode) const override {
    if (UnknownCalls.count(CallNode)) {
      return std::nullopt;
    }
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
  std::set<Node> UnknownCalls;
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

  void registerContextForTest(
      std::shared_ptr<vasco::Context<Method, Node, Sign>> Context) {
    this->registerContext(std::move(Context));
  }

  void runSanityCheckForTest() const { this->sanityCheckAnalysedContexts(); }

protected:
  Sign normalFlowFunction(ContextPtr, const Node &NodeName,
                          const Sign &InValue) override {
    if (NodeName == "main.assign") {
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

  Sign callLocalFlowFunction(ContextPtr, const Node &, const Sign &InValue) override {
    return InValue;
  }

private:
  const ToyProgramRepresentation &Program;
};

using FactSet = std::set<std::string>;

class BackwardRequirementAnalysis final
    : public vasco::BackwardInterProceduralAnalysis<Method, Node, FactSet> {
public:
  explicit BackwardRequirementAnalysis(const ToyProgramRepresentation &Program)
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
  FactSet normalFlowFunction(ContextPtr, const Node &NodeName,
                             const FactSet &OutValue) override {
    FactSet Result = OutValue;
    if (NodeName == "main.return") {
      if (Result.count("ret")) {
        Result.erase("ret");
        Result.insert("x");
      }
      return Result;
    }
    if (NodeName == "callee.return") {
      if (Result.count("ret")) {
        Result.erase("ret");
        Result.insert("a");
      }
      return Result;
    }
    return Result;
  }

  FactSet callEntryFlowFunction(ContextPtr, const Method &, const Node &,
                                const FactSet &EntryValue) override {
    FactSet Result;
    if (EntryValue.count("a")) {
      Result.insert("a");
    }
    return Result;
  }

  FactSet callExitFlowFunction(ContextPtr, const Method &, const Node &,
                               const FactSet &OutValue) override {
    FactSet Result;
    if (OutValue.count("x")) {
      Result.insert("ret");
    }
    return Result;
  }

  FactSet callLocalFlowFunction(ContextPtr, const Node &,
                                const FactSet &OutValue) override {
    FactSet Result = OutValue;
    Result.erase("x");
    return Result;
  }

private:
  const ToyProgramRepresentation &Program;
};

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
  FactSet normalFlowFunction(ContextPtr, const Node &, const FactSet &OutValue) override {
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

TEST(VASCOTest, ForwardReusesEquivalentCalleeContexts) {
  ToyProgramRepresentation Program;
  Program.addMethod("main", std::make_shared<ToyGraph>(
                                std::vector<Node>{"main.assign", "main.call1",
                                                  "main.call2", "main.exit"},
                                std::vector<Node>{"main.assign"},
                                std::vector<Node>{"main.exit"},
                                std::vector<std::pair<Node, Node>>{
                                    {"main.assign", "main.call1"},
                                    {"main.call1", "main.call2"},
                                    {"main.call2", "main.exit"}}));
  Program.addMethod("callee", std::make_shared<ToyGraph>(
                                  std::vector<Node>{"callee.exit"},
                                  std::vector<Node>{"callee.exit"},
                                  std::vector<Node>{"callee.exit"},
                                  std::vector<std::pair<Node, Node>>{}));
  Program.addEntryPoint("main");
  Program.addCall("main.call1", {"callee"});
  Program.addCall("main.call2", {"callee"});

  ForwardSignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  const auto &Contexts = Analysis.getContexts("callee");
  ASSERT_EQ(Contexts.size(), 1U);
  EXPECT_EQ(Contexts.front()->getEntryValue(), Sign::Positive);
  EXPECT_EQ(Contexts.front()->getExitValue(), Sign::Positive);

  const auto *Callers = Analysis.getCallers(Contexts.front());
  ASSERT_NE(Callers, nullptr);
  EXPECT_EQ(Callers->size(), 2U);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(Solution.getValueAfter("main.call2"), Sign::Positive);
  EXPECT_EQ(Solution.getValueAfter("main.exit"), Sign::Positive);
}

TEST(VASCOTest, ForwardTerminatesOnRecursiveValueContexts) {
  ToyProgramRepresentation Program;
  Program.addMethod("main", std::make_shared<ToyGraph>(
                                std::vector<Node>{"main.assign", "main.call",
                                                  "main.exit"},
                                std::vector<Node>{"main.assign"},
                                std::vector<Node>{"main.exit"},
                                std::vector<std::pair<Node, Node>>{
                                    {"main.assign", "main.call"},
                                    {"main.call", "main.exit"}}));
  Program.addMethod("rec", std::make_shared<ToyGraph>(
                               std::vector<Node>{"rec.call", "rec.return"},
                               std::vector<Node>{"rec.call"},
                               std::vector<Node>{"rec.return"},
                               std::vector<std::pair<Node, Node>>{
                                   {"rec.call", "rec.return"}}));
  Program.addEntryPoint("main");
  Program.addCall("main.call", {"rec"});
  Program.addCall("rec.call", {"rec"});

  ForwardSignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  const auto &Contexts = Analysis.getContexts("rec");
  ASSERT_EQ(Contexts.size(), 1U);
  EXPECT_TRUE(Contexts.front()->isAnalysed());
  EXPECT_EQ(Contexts.front()->getEntryValue(), Sign::Positive);
  EXPECT_EQ(Contexts.front()->getExitValue(), Sign::Positive);

  const auto Reachable =
      Analysis.getContextTransitionTable().reachableSet(Contexts.front(), false);
  EXPECT_EQ(Reachable.size(), 1U);
  EXPECT_TRUE(Reachable.count(Contexts.front()));
}

TEST(VASCOTest, BackwardPropagatesRequirementsAcrossCalls) {
  ToyProgramRepresentation Program;
  Program.addMethod("main", std::make_shared<ToyGraph>(
                                std::vector<Node>{"main.call", "main.return"},
                                std::vector<Node>{"main.call"},
                                std::vector<Node>{"main.return"},
                                std::vector<std::pair<Node, Node>>{
                                    {"main.call", "main.return"}}));
  Program.addMethod("callee", std::make_shared<ToyGraph>(
                                  std::vector<Node>{"callee.return"},
                                  std::vector<Node>{"callee.return"},
                                  std::vector<Node>{"callee.return"},
                                  std::vector<std::pair<Node, Node>>{}));
  Program.addEntryPoint("main");
  Program.addCall("main.call", {"callee"});

  BackwardRequirementAnalysis Analysis(Program);
  Analysis.doAnalysis();

  const auto &CalleeContexts = Analysis.getContexts("callee");
  ASSERT_EQ(CalleeContexts.size(), 1U);
  EXPECT_EQ(CalleeContexts.front()->getExitValue(), (FactSet{"ret"}));
  EXPECT_EQ(CalleeContexts.front()->getEntryValue(), (FactSet{"a"}));

  const auto &MainContexts = Analysis.getContexts("main");
  ASSERT_EQ(MainContexts.size(), 1U);
  EXPECT_EQ(MainContexts.front()->getEntryValue(), (FactSet{"a"}));
}

TEST(VASCOTest, TransitionTableTracksDefaultSites) {
  using ContextType = vasco::Context<Method, Node, Sign>;
  using ContextPtr = std::shared_ptr<ContextType>;

  vasco::ContextTransitionTable<Method, Node, Sign> Table;
  auto Caller = std::make_shared<ContextType>("main");
  vasco::CallSite<Method, Node, Sign> Site(Caller, "main.call");

  Table.addTransition(Site, nullptr);

  EXPECT_EQ(Table.getDefaultCallSites().size(), 1U);
  EXPECT_TRUE(Table.getTargets(Site) == nullptr);
}

TEST(VASCOTest, ForwardMarksUnknownTargetsAsDefaultSites) {
  ToyProgramRepresentation Program;
  Program.addMethod("main", std::make_shared<ToyGraph>(
                                std::vector<Node>{"main.call", "main.exit"},
                                std::vector<Node>{"main.call"},
                                std::vector<Node>{"main.exit"},
                                std::vector<std::pair<Node, Node>>{
                                    {"main.call", "main.exit"}}));
  Program.addEntryPoint("main");
  Program.addUnknownCall("main.call");

  ForwardSignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  const auto &Defaults =
      Analysis.getContextTransitionTable().getDefaultCallSites();
  ASSERT_EQ(Defaults.size(), 1U);
  EXPECT_EQ(Defaults.begin()->getCallNode(), "main.call");
}

TEST(VASCOTest, MeetOverValidPathsRejectsFreedContexts) {
  ToyProgramRepresentation Program;
  Program.addMethod("main", std::make_shared<ToyGraph>(
                                std::vector<Node>{"main.assign", "main.exit"},
                                std::vector<Node>{"main.assign"},
                                std::vector<Node>{"main.exit"},
                                std::vector<std::pair<Node, Node>>{
                                    {"main.assign", "main.exit"}}));
  Program.addEntryPoint("main");

  ForwardSignAnalysis Analysis(Program);
  Analysis.setFreeResultsOnTheFly(true);
  Analysis.doAnalysis();

  EXPECT_THROW(
      {
        auto Solution = Analysis.getMeetOverValidPathsSolution();
        (void)Solution;
      },
      std::logic_error);
}

TEST(VASCOTest, BackwardTreatsEmptyTargetCallAsLocalFlow) {
  ToyProgramRepresentation Program;
  Program.addMethod("main", std::make_shared<ToyGraph>(
                                std::vector<Node>{"main.call", "main.return"},
                                std::vector<Node>{"main.call"},
                                std::vector<Node>{"main.return"},
                                std::vector<std::pair<Node, Node>>{
                                    {"main.call", "main.return"}}));
  Program.addEntryPoint("main");
  Program.addCall("main.call", {});

  BackwardIdentityAnalysis Analysis(Program);
  Analysis.doAnalysis();

  const auto &MainContexts = Analysis.getContexts("main");
  ASSERT_EQ(MainContexts.size(), 1U);
  EXPECT_EQ(MainContexts.front()->getEntryValue(), (FactSet{"ret"}));
}

TEST(VASCOTest, SanityCheckReportsPartialContexts) {
  ToyProgramRepresentation Program;
  Program.addMethod("main", std::make_shared<ToyGraph>(
                                std::vector<Node>{"main.assign", "main.exit"},
                                std::vector<Node>{"main.assign"},
                                std::vector<Node>{"main.exit"},
                                std::vector<std::pair<Node, Node>>{
                                    {"main.assign", "main.exit"}}));
  Program.addEntryPoint("main");

  ForwardSignAnalysis Analysis(Program);
  auto Context = std::make_shared<vasco::Context<Method, Node, Sign>>("main");
  Analysis.registerContextForTest(Context);

  std::stringstream Err;
  auto *Old = std::cerr.rdbuf(Err.rdbuf());
  Analysis.runSanityCheckForTest();
  std::cerr.rdbuf(Old);

  EXPECT_NE(Err.str().find("Only partial analysis of X"), std::string::npos);
  EXPECT_NE(Err.str().find("main"), std::string::npos);
}

} // namespace
