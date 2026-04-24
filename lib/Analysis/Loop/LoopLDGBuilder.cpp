/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopLDGBuilder.h"

#include "Analysis/Loop/InductionVariables.h"
#include "Analysis/Loop/Invariants.h"
#include "Analysis/Loop/LoopCarriedDependencies.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "Analysis/Loop/LoopIterationSpaceAnalysis.h"
#include "Analysis/Loop/MemoryCloningAnalysis.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

#include <queue>
#include <unordered_set>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

std::string describeValue(Value *value) {
  if (value == nullptr) {
    return "null";
  }
  if (value->hasName()) {
    return value->getName().str();
  }
  std::string text;
  raw_string_ostream stream(text);
  value->printAsOperand(stream, false);
  return stream.str();
}

bool shouldIgnoreLoopInstruction(Instruction *instruction) {
  if (instruction == nullptr) {
    return true;
  }
  if (isa<DbgInfoIntrinsic>(instruction)) {
    return true;
  }
  if (auto *call = dyn_cast<CallInst>(instruction)) {
    if (call->isLifetimeStartOrEnd()) {
      return true;
    }
  }
  return false;
}

bool shouldIgnoreValue(Value *value) {
  if (value == nullptr) {
    return true;
  }
  if (isa<BasicBlock>(value) || isa<MetadataAsValue>(value)) {
    return true;
  }
  if (auto *inst = dyn_cast<Instruction>(value)) {
    return shouldIgnoreLoopInstruction(inst);
  }
  return false;
}

bool isAllowedBoundaryContextValue(Value *value) {
  if (value == nullptr || shouldIgnoreValue(value)) {
    return false;
  }
  return true;
}

bool isInternalToLoop(LoopStructure *loop, Value *value) {
  if (loop == nullptr || value == nullptr) {
    return false;
  }
  if (auto *instruction = dyn_cast<Instruction>(value)) {
    return loop->isIncluded(instruction);
  }
  return false;
}

bool canInstructionReachWithinSameIteration(const LoopStructure *loopStructure,
                                            Instruction *from,
                                            Instruction *to) {
  if (loopStructure == nullptr || from == nullptr || to == nullptr) {
    return false;
  }
  if (!loopStructure->isIncluded(from) || !loopStructure->isIncluded(to)) {
    return false;
  }

  if (from->getParent() == to->getParent()) {
    bool seenFrom = false;
    for (auto &inst : *from->getParent()) {
      if (&inst == from) {
        seenFrom = true;
      }
      if (seenFrom && &inst == to) {
        return true;
      }
    }
    return false;
  }

  std::queue<BasicBlock *> worklist;
  std::unordered_set<BasicBlock *> visited;
  worklist.push(from->getParent());
  visited.insert(from->getParent());
  auto *header = loopStructure->getHeader();

  while (!worklist.empty()) {
    auto *current = worklist.front();
    worklist.pop();
    for (auto *succ : successors(current)) {
      if (!loopStructure->isIncluded(succ) || succ == header) {
        continue;
      }
      if (succ == to->getParent()) {
        return true;
      }
      if (visited.insert(succ).second) {
        worklist.push(succ);
      }
    }
  }

  return false;
}

class LoopAwareDependenceRefinementPass {
public:
  virtual ~LoopAwareDependenceRefinementPass() = default;

  virtual void refine(LoopDependenceGraph &,
                      LoopTree *,
                      llvm::ScalarEvolution &,
                      llvm::LoopInfo &,
                      const noelle::DominatorSummary &) = 0;
};

bool isReadOnlyInstruction(const Instruction *instruction) {
  if (instruction == nullptr) {
    return false;
  }

  if (isa<LoadInst>(instruction)) {
    return true;
  }

  auto *call = dyn_cast<CallBase>(instruction);
  if (call == nullptr) {
    return !instruction->mayWriteToMemory();
  }

  if (call->mayWriteToMemory()) {
    return false;
  }

  return true;
}

class ImportedPDGLoopAwareDependenceRefinementPass
    : public LoopAwareDependenceRefinementPass {
public:
  void refine(LoopDependenceGraph &graph,
              LoopTree *loopNode,
              llvm::ScalarEvolution &,
              llvm::LoopInfo &,
              const noelle::DominatorSummary &DS) override {
    std::vector<LoopDependenceEdge *> edgesToRemove;
    for (auto *edge : graph.getEdges()) {
      if (edge->getKind() != LoopDependenceEdgeKind::Memory ||
          edge->getOrigin() != LoopDependenceEdgeOrigin::ImportedPDG) {
        continue;
      }

      auto *producer = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
      auto *consumer = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
      if (producer == nullptr || consumer == nullptr) {
        continue;
      }

      // DATA_READ and load/load alias edges are not true data dependences.
      if (edge->getMemoryKind() == LoopDependenceMemoryKind::ReadOnly ||
          (isReadOnlyInstruction(producer) && isReadOnlyInstruction(consumer))) {
        edgesToRemove.push_back(edge);
      }
    }

    for (auto *edge : edgesToRemove) {
      edge->setLoopCarried(false);
      graph.removeEdge(edge);
    }

    LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS, graph);
  }
};

std::vector<Value *> collectLoopInternalValues(LoopStructure *loopStructure) {
  std::vector<Value *> values;
  if (loopStructure == nullptr) {
    return values;
  }

  for (auto *block : loopStructure->getBasicBlocks()) {
    for (auto &instruction : *block) {
      if (shouldIgnoreLoopInstruction(&instruction)) {
        continue;
      }
      values.push_back(&instruction);
    }
  }

  return values;
}

void removeLoopCarriedDependencesProvedDisjoint(
    LoopDependenceGraph &graph,
    LoopStructure *loopStructure,
    LoopIterationSpaceAnalysis &analysis) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *from = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
    auto *to = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (from == nullptr || to == nullptr) {
      continue;
    }
    if (canInstructionReachWithinSameIteration(loopStructure, from, to)) {
      continue;
    }
    if (!analysis
             .areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
                 from, to)) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
}

class AffineLoopDependenceRefinementPass
    : public LoopAwareDependenceRefinementPass {
public:
  void refine(LoopDependenceGraph &graph,
              LoopTree *loopNode,
              llvm::ScalarEvolution &SE,
              llvm::LoopInfo &LI,
              const noelle::DominatorSummary &DS) override {
    InvariantManager temporaryInvariants(loopNode->getLoop(), &graph);
    auto loopInternalValues = collectLoopInternalValues(loopNode->getLoop());
    auto temporaryLoopInternalGraph = graph.createSubgraphFromValues(
        loopInternalValues, /*linkToExternal=*/false);
    auto temporaryIVDependenceGraph =
        temporaryLoopInternalGraph->createSubgraph(/*includeControl=*/true,
                                                   /*includeVariable=*/true,
                                                   /*includeMemory=*/false);
    auto temporaryIVSCCDAG =
        std::unique_ptr<LoopSCCDAG>(new LoopSCCDAG(*temporaryIVDependenceGraph));
    LoopEnvironment temporaryEnvironment(
        &graph, loopNode->getLoop()->getLoopExitBasicBlocks());
    auto temporaryIVs = std::unique_ptr<InductionVariableManager>(
        new InductionVariableManager(loopNode,
                                     temporaryInvariants,
                                     SE,
                                     LI,
                                     *temporaryIVSCCDAG,
                                     temporaryEnvironment));
    LoopIterationSpaceAnalysis temporaryIterationSpace(loopNode,
                                                       *temporaryIVs,
                                                       SE);
    removeLoopCarriedDependencesProvedDisjoint(
        graph, loopNode->getLoop(), temporaryIterationSpace);
    LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS, graph);
  }
};

lotus::alias::AliasSpecManager &getLoopAnalysisSpecManager(void) {
  static lotus::alias::AliasSpecManager manager;
  return manager;
}

bool isThreadSafeLibraryCall(CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  auto *callee = call->getCalledFunction();
  if (callee == nullptr || !callee->empty()) {
    return false;
  }

  auto &specManager = getLoopAnalysisSpecManager();
  if (specManager.isAllocator(callee) || specManager.isDeallocator(callee) ||
      specManager.getCategory(callee) ==
          lotus::alias::FunctionCategory::Reallocator) {
    return true;
  }

  static const std::set<std::string> noelleThreadSafeFallback{
      "malloc", "calloc", "realloc", "free"};
  return noelleThreadSafeFallback.count(callee->getName().str()) != 0;
}

void removeThreadSafeLibraryDependences(LoopDependenceGraph &graph) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *producer = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
    auto *consumer = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (producer == nullptr || consumer == nullptr || producer != consumer) {
      continue;
    }

    auto *call = dyn_cast<CallInst>(producer);
    if (!isThreadSafeLibraryCall(call)) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
}

void removeMemoryCloningNegatedDependences(LoopDependenceGraph &graph,
                                           MemoryCloningAnalysis &analysis) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *producer = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
    auto *consumer = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (producer == nullptr || consumer == nullptr) {
      continue;
    }

    auto locationsProducer = analysis.getClonableMemoryObjectsFor(producer);
    auto locationsConsumer = analysis.getClonableMemoryObjectsFor(consumer);
    if (locationsProducer.empty() || locationsConsumer.empty()) {
      continue;
    }

    bool removable = false;
    for (auto *locationProducer : locationsProducer) {
      for (auto *locationConsumer : locationsConsumer) {
        if (locationProducer->getAllocation() !=
            locationConsumer->getAllocation()) {
          continue;
        }
        bool producerStores =
            locationProducer->isInstructionStoringLocation(producer);
        bool consumerStores =
            locationConsumer->isInstructionStoringLocation(consumer);
        bool producerLoads =
            locationProducer->isInstructionLoadingLocation(producer);
        bool consumerLoads =
            locationConsumer->isInstructionLoadingLocation(consumer);

        bool isRAW = producerStores && consumerLoads;
        bool isWAR = producerLoads && consumerStores;
        bool isWAW = producerStores && consumerStores;
        if (isRAW || isWAR || isWAW) {
          removable = true;
          break;
        }
      }
      if (removable) {
        break;
      }
    }

    if (!removable) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
}

} // namespace

void LoopLDGBuilder::captureSnapshot(GraphBundle &bundle,
                                     const std::string &phase,
                                     const LoopDependenceGraph &graph) {
  bundle.debugSnapshots.emplace_back(phase, graph.renderStable());
}

LoopLDGBuilder::GraphBundle LoopLDGBuilder::buildBaseLoopDependenceGraph(
    LoopTree *loopNode,
    pdg::ProgramGraph &pdg) {
  GraphBundle bundle;
  bundle.graph.reset(new LoopDependenceGraph());
  auto &graph = *bundle.graph;
  graph.initialize(loopNode, pdg);

  auto *loopStructure = graph.getLoopStructure();
  assert(loopStructure != nullptr);

  std::vector<Value *> loopInternalValues = collectLoopInternalValues(loopStructure);
  std::vector<Instruction *> loopInstructions;
  std::unordered_set<Value *> loopInternalValueSet;
  loopInstructions.reserve(loopInternalValues.size());
  for (auto *value : loopInternalValues) {
    loopInternalValueSet.insert(value);
    if (auto *instruction = dyn_cast<Instruction>(value)) {
      loopInstructions.push_back(instruction);
    }
  }

  std::sort(loopInstructions.begin(), loopInstructions.end(),
            [](Instruction *lhs, Instruction *rhs) {
              if (lhs->getParent()->getName() != rhs->getParent()->getName()) {
                return lhs->getParent()->getName() < rhs->getParent()->getName();
              }
              return describeValue(lhs) < describeValue(rhs);
            });

  for (auto *instruction : loopInstructions) {
    auto *pdgNode = pdg.getNode(*instruction);
    graph.fetchOrCreateNode(instruction, pdgNode, true);
  }

  std::set<std::tuple<pdg::Node *, pdg::Node *, int>> seenEdges;
  for (auto *instruction : loopInstructions) {
    auto *pdgNode = pdg.getNode(*instruction);
    if (pdgNode == nullptr) {
      continue;
    }

    for (auto *edge : pdgNode->getOutEdgeSet()) {
      graph.importEdgeIfIncluded(edge, /*linkToExternal=*/true,
                                 loopInternalValueSet, seenEdges);
    }

    for (auto *edge : pdgNode->getInEdgeSet()) {
      graph.importEdgeIfIncluded(edge, /*linkToExternal=*/true,
                                 loopInternalValueSet, seenEdges);
    }
  }

  captureSnapshot(bundle, "base", graph);
  bundle.sccdag.reset(new LoopSCCDAG(graph));
  return bundle;
}

LoopLDGBuilder::GraphBundle LoopLDGBuilder::refineLoopDependenceGraph(
    std::unique_ptr<LoopDependenceGraph> graph,
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    const noelle::DominatorSummary &DS,
    LoopLDGBuilderOptions options) {
  GraphBundle bundle;
  bundle.graph = std::move(graph);
  auto &resolvedGraph = *bundle.graph;
  auto *loopNode = resolvedGraph.getLoopHierarchyStructures();
  assert(loopNode != nullptr);

  LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS, resolvedGraph);

  if (options.enableLoopAwareDependenceAnalyses &&
      options.hasLoopAwareDependenceBackend) {
    ImportedPDGLoopAwareDependenceRefinementPass loopAwarePass;
    loopAwarePass.refine(resolvedGraph, loopNode, SE, LI, DS);
  }
  captureSnapshot(bundle, "after_loop_aware", resolvedGraph);

  if (options.enableAffineIterationSpaceRefinement) {
    AffineLoopDependenceRefinementPass affinePass;
    affinePass.refine(resolvedGraph, loopNode, SE, LI, DS);
  }
  captureSnapshot(bundle, "after_affine", resolvedGraph);

  if (options.enableMemoryCloningRefinement) {
    MemoryCloningAnalysis memoryCloning(loopNode->getLoop(),
                                        const_cast<noelle::DominatorSummary &>(DS),
                                        &resolvedGraph);
    removeMemoryCloningNegatedDependences(resolvedGraph, memoryCloning);
    LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS, resolvedGraph);
  }
  captureSnapshot(bundle, "after_memory_cloning", resolvedGraph);

  if (options.enableThreadSafeLibraryRefinement) {
    removeThreadSafeLibraryDependences(resolvedGraph);
    LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS, resolvedGraph);
  }
  captureSnapshot(bundle, "after_thread_safe_library", resolvedGraph);

  bundle.sccdag.reset(new LoopSCCDAG(resolvedGraph));
  LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS, resolvedGraph);
  captureSnapshot(bundle, "final", resolvedGraph);

  return bundle;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
