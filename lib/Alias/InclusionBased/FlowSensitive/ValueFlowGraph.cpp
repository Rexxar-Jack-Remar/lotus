#include "Alias/InclusionBased/FlowSensitive/ValueFlowGraph.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lotus::alias::vfg {
namespace {
template <typename T> void appendUnique(std::vector<T> &values, T value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}
template <typename T> bool unite(std::set<T> &to, const std::set<T> &from) {
  const auto before = to.size();
  to.insert(from.begin(), from.end());
  return before != to.size();
}
std::uint32_t nextID(std::size_t size) {
  if (size >= std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("ValueFlowPTA: ID space exhausted");
  return static_cast<std::uint32_t>(size);
}
} // namespace

Program::Program() {
  addObject(false, false, "<null>");
  addObject(false, true, "<unknown>");
}
void Program::checkNode(NodeID id) const {
  if (!id || id >= edges_.size())
    throw std::out_of_range("ValueFlowPTA: invalid value node");
}
void Program::checkBlock(BlockID id) const {
  if (!id || id >= blocks_.size())
    throw std::out_of_range("ValueFlowPTA: invalid control block");
}
NodeID Program::addNode() {
  const NodeID id = nextID(edges_.size());
  edges_.emplace_back();
  sources_.push_back(false);
  return id;
}
ObjectID Program::addObject(bool singleton, bool memory, std::string name) {
  const ObjectID id = nextID(objects_.size());
  const NodeID source = addNode();
  sources_[source] = true;
  const NodeID initial = memory ? addNode() : 0;
  objects_.push_back({source, initial, singleton, memory, std::move(name)});
  initializers_.push_back(0);
  return id;
}
BlockID Program::addBlock() {
  const BlockID id = nextID(blocks_.size());
  blocks_.emplace_back();
  return id;
}
void Program::addRoot(BlockID block) {
  checkBlock(block);
  roots_.insert(block);
}
void Program::addControlEdge(BlockID from, BlockID to) {
  checkBlock(from);
  checkBlock(to);
  appendUnique(blocks_[from].successors, to);
}
void Program::addCopy(NodeID from, NodeID to) {
  checkNode(from);
  checkNode(to);
  if (sources_[to])
    throw std::invalid_argument(
        "ValueFlowPTA: object addresses are source nodes");
  appendUnique(edges_[from], to);
}
AccessID Program::addLoad(BlockID block, NodeID pointer, NodeID result) {
  checkBlock(block);
  checkNode(pointer);
  checkNode(result);
  if (sources_[result])
    throw std::invalid_argument(
        "ValueFlowPTA: load result is an object source");
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Load, block, pointer, result, false});
  blocks_[block].accesses.push_back(id);
  return id;
}
AccessID Program::addStore(BlockID block, NodeID pointer, NodeID source,
                           bool allowStrong) {
  checkBlock(block);
  checkNode(pointer);
  checkNode(source);
  const NodeID stored = addNode();
  addCopy(source, stored);
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Store, block, pointer, stored, allowStrong});
  blocks_[block].accesses.push_back(id);
  return id;
}
void Program::setInitializer(ObjectID object, NodeID value) {
  checkNode(value);
  if (!object || object >= objects_.size() || !objects_[object].memory)
    throw std::invalid_argument(
        "ValueFlowPTA: initializer needs a memory object");
  if (initializers_[object])
    throw std::invalid_argument("ValueFlowPTA: initializer already set");
  initializers_[object] = value;
}
void Program::setSingleton(ObjectID object, bool singleton) {
  if (!object || object >= objects_.size() || object <= unknownObject())
    throw std::invalid_argument("ValueFlowPTA: invalid singleton object");
  objects_[object].singleton = singleton;
}

class Solver::Impl {
public:
  Impl(const Program &program, Config config) : p(program), config(config) {}
  const Program &p;
  Config config;
  Statistics stats;
  bool weak = false;
  bool analyzed = false;
  std::vector<std::vector<NodeID>> edges;
  std::vector<PointsToSet> pts;
  std::vector<NodeSet> pted;
  std::vector<bool> reachable;
  std::vector<std::vector<AccessID>> uses;
  std::vector<NodeID> storedThrough;
  std::vector<ObjectID> initializedObject;
  std::set<ObjectID> dirtyReach;
  std::set<ObjectID> pending;
  std::vector<bool> processed;
  std::set<AccessID> finalStrong;

  using CFGNode = std::size_t;
  static constexpr CFGNode InvalidCFGNode = std::numeric_limits<CFGNode>::max();
  std::vector<std::vector<CFGNode>> cfgSuccessors;
  std::vector<std::vector<CFGNode>> cfgPredecessors;
  std::vector<std::vector<CFGNode>> dominatorChildren;
  std::vector<std::vector<CFGNode>> dominanceFrontiers;
  std::vector<CFGNode> immediateDominator;
  std::vector<CFGNode> accessLocation;
  std::vector<AccessID> locationAccess;

  struct Signature {
    std::set<AccessID> accesses;
    std::set<AccessID> strong;
    bool operator==(const Signature &other) const {
      return accesses == other.accesses && strong == other.strong;
    }
  };
  std::vector<Signature> previous;

  void buildDominanceInfo() {
    cfgSuccessors.assign(1, {});
    cfgPredecessors.assign(1, {});
    locationAccess.assign(1, 0);
    accessLocation.assign(p.accesses_.size(), InvalidCFGNode);
    std::vector<CFGNode> blockEntry(p.blocks_.size(), InvalidCFGNode);
    std::vector<CFGNode> blockExit(p.blocks_.size(), InvalidCFGNode);

    auto addNode = [&](AccessID access = 0) {
      const CFGNode node = cfgSuccessors.size();
      cfgSuccessors.emplace_back();
      cfgPredecessors.emplace_back();
      locationAccess.push_back(access);
      return node;
    };
    auto addEdge = [&](CFGNode from, CFGNode to) {
      appendUnique(cfgSuccessors[from], to);
      appendUnique(cfgPredecessors[to], from);
    };

    for (BlockID block = 1; block < p.blocks_.size(); ++block) {
      if (!reachable[block])
        continue;
      CFGNode current = addNode();
      blockEntry[block] = current;
      for (AccessID access : p.blocks_[block].accesses) {
        const CFGNode location = addNode(access);
        accessLocation[access] = location;
        addEdge(current, location);
        current = location;
      }
      blockExit[block] = current;
    }
    for (BlockID block = 1; block < p.blocks_.size(); ++block) {
      if (!reachable[block])
        continue;
      for (BlockID successor : p.blocks_[block].successors)
        if (reachable[successor])
          addEdge(blockExit[block], blockEntry[successor]);
    }
    for (BlockID root : p.roots_)
      if (reachable[root])
        addEdge(0, blockEntry[root]);

    std::vector<bool> visited(cfgSuccessors.size(), false);
    std::vector<std::pair<CFGNode, std::size_t>> dfs{{0, 0}};
    std::vector<CFGNode> postorder;
    visited[0] = true;
    while (!dfs.empty()) {
      auto &[node, successorIndex] = dfs.back();
      if (successorIndex < cfgSuccessors[node].size()) {
        const CFGNode successor = cfgSuccessors[node][successorIndex++];
        if (!visited[successor]) {
          visited[successor] = true;
          dfs.emplace_back(successor, 0);
        }
        continue;
      }
      postorder.push_back(node);
      dfs.pop_back();
    }
    std::vector<CFGNode> reversePostorder(postorder.rbegin(), postorder.rend());
    std::vector<CFGNode> rpoNumber(cfgSuccessors.size(), InvalidCFGNode);
    for (CFGNode number = 0; number < reversePostorder.size(); ++number)
      rpoNumber[reversePostorder[number]] = number;

    immediateDominator.assign(cfgSuccessors.size(), InvalidCFGNode);
    immediateDominator[0] = 0;
    auto intersect = [&](CFGNode left, CFGNode right) {
      while (left != right) {
        while (rpoNumber[left] > rpoNumber[right])
          left = immediateDominator[left];
        while (rpoNumber[right] > rpoNumber[left])
          right = immediateDominator[right];
      }
      return left;
    };
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t index = 1; index < reversePostorder.size(); ++index) {
        const CFGNode node = reversePostorder[index];
        CFGNode newDominator = InvalidCFGNode;
        for (CFGNode predecessor : cfgPredecessors[node]) {
          if (immediateDominator[predecessor] == InvalidCFGNode)
            continue;
          newDominator = newDominator == InvalidCFGNode
                             ? predecessor
                             : intersect(predecessor, newDominator);
        }
        if (newDominator == InvalidCFGNode)
          throw std::logic_error(
              "ValueFlowPTA: reachable control node has no dominator");
        if (immediateDominator[node] != newDominator) {
          immediateDominator[node] = newDominator;
          changed = true;
        }
      }
    }

    dominatorChildren.assign(cfgSuccessors.size(), {});
    for (CFGNode node = 1; node < cfgSuccessors.size(); ++node)
      dominatorChildren[immediateDominator[node]].push_back(node);
    dominanceFrontiers.assign(cfgSuccessors.size(), {});
    for (CFGNode node = 1; node < cfgSuccessors.size(); ++node) {
      if (cfgPredecessors[node].size() < 2)
        continue;
      for (CFGNode predecessor : cfgPredecessors[node]) {
        CFGNode runner = predecessor;
        while (runner != immediateDominator[node]) {
          appendUnique(dominanceFrontiers[runner], node);
          runner = immediateDominator[runner];
        }
      }
    }
    stats.expandedControlFlowNodes = cfgSuccessors.size();
  }

  void reset(bool weakMode) {
    weak = weakMode;
    edges = p.edges_;
    pts.assign(p.edges_.size(), {});
    pted.assign(p.objects_.size(), {});
    uses.assign(p.edges_.size(), {});
    storedThrough.assign(p.edges_.size(), 0);
    initializedObject.assign(p.edges_.size(), 0);
    previous.assign(p.objects_.size(), {});
    processed.assign(p.objects_.size(), false);
    reachable.assign(p.blocks_.size(), false);
    dirtyReach.clear();
    pending.clear();
    finalStrong.clear();
    stats.indirectEdges = 0;
    stats.strongUpdateSites = 0;

    std::vector<BlockID> work(p.roots_.begin(), p.roots_.end());
    while (!work.empty()) {
      const BlockID block = work.back();
      work.pop_back();
      if (reachable[block])
        continue;
      reachable[block] = true;
      work.insert(work.end(), p.blocks_[block].successors.begin(),
                  p.blocks_[block].successors.end());
    }
    for (AccessID id = 1; id < p.accesses_.size(); ++id) {
      const auto &access = p.accesses_[id];
      if (!reachable[access.block])
        continue;
      uses[access.pointer].push_back(id);
      if (access.kind == Program::AccessKind::Store)
        storedThrough[access.value] = access.pointer;
    }
    buildDominanceInfo();
    for (ObjectID object = 1; object < p.objects_.size(); ++object) {
      dirtyReach.insert(object);
      if (!p.objects_[object].memory)
        continue;
      pending.insert(object);
      // A virtual initial store participates both in reachability and in
      // escape ordering, including global pointer initializers.
      const NodeID initializer =
          p.initializers_[object] ? p.initializers_[object] : p.unknownValue();
      appendUnique(edges[initializer], p.objects_[object].initialStore);
      initializedObject[p.objects_[object].initialStore] = object;
    }
  }

  // Algorithm 1: traverse once per affected object, rather than propagating
  // whole points-to sets through the control-flow graph.
  void computePointedToBy() {
    while (!dirtyReach.empty()) {
      const ObjectID object = *dirtyReach.begin();
      dirtyReach.erase(dirtyReach.begin());
      ++stats.objectTraversals;
      NodeSet visited;
      std::vector<NodeID> work{p.objects_[object].address};
      while (!work.empty()) {
        const NodeID node = work.back();
        work.pop_back();
        if (!visited.insert(node).second)
          continue;
        pted[object].insert(node);
        pts[node].insert(object);
        work.insert(work.end(), edges[node].begin(), edges[node].end());
      }
    }
  }

  Signature signature(ObjectID object) const {
    Signature result;
    auto collect = [&](ObjectID from) {
      for (NodeID pointer : pted[from]) {
        for (AccessID id : uses[pointer]) {
          result.accesses.insert(id);
          const auto &access = p.accesses_[id];
          if (!weak && p.objects_[object].singleton && access.allowStrong &&
              access.kind == Program::AccessKind::Store &&
              pts[pointer].size() == 1 && pts[pointer].count(object))
            result.strong.insert(id);
        }
      }
    };
    collect(object);
    // Unknown is a wildcard address, not a disjoint allocation site.
    if (object != p.unknownObject())
      collect(p.unknownObject());
    return result;
  }

  struct EscapeSet {
    PointsToSet objects;
    bool unresolved = false;
  };
  EscapeSet escapes(ObjectID object) const {
    EscapeSet result;
    // Examine only stores reached by this object, not every store in the
    // module. The store node holds the stored value; its pointer names the
    // destination. Reversing the two reverses the escape order.
    for (NodeID node : pted[object]) {
      if (initializedObject[node]) {
        result.objects.insert(initializedObject[node]);
        continue;
      }
      const NodeID pointer = storedThrough[node];
      if (!pointer)
        continue;
      const auto &targets = pts[pointer];
      if (targets.empty()) {
        result.unresolved = true;
      } else if (targets.count(p.unknownObject())) {
        for (ObjectID target = 1; target < p.objects_.size(); ++target)
          if (p.objects_[target].memory)
            result.objects.insert(target);
      } else {
        for (ObjectID target : targets)
          if (p.objects_[target].memory)
            result.objects.insert(target);
      }
    }
    return result;
  }

  bool addIndirect(NodeID from, NodeID to) {
    auto &successors = edges[from];
    if (std::find(successors.begin(), successors.end(), to) != successors.end())
      return false;
    successors.push_back(to);
    ++stats.indirectEdges;
    // Algorithm 3, UpdateVFG: only objects reaching the source need traversal.
    dirtyReach.insert(pts[from].begin(), pts[from].end());
    return true;
  }

  // Algorithm 2's store-plus-IDF construction, applied to the expanded
  // interprocedural control graph. Expanding blocks at memory accesses makes
  // dominance sensitive to instruction order within an LLVM basic block.
  void computeIndirect(ObjectID object, const Signature &sig) {
    ++stats.indirectFlowComputations;
    stats.projectedEvents += sig.accesses.size();
    if (sig.accesses.empty())
      return;

    std::vector<bool> storeLocation(cfgSuccessors.size(), false);
    std::deque<CFGNode> idfWork;
    for (AccessID access : sig.accesses) {
      if (p.accesses_[access].kind != Program::AccessKind::Store)
        continue;
      const CFGNode location = accessLocation[access];
      storeLocation[location] = true;
      idfWork.push_back(location);
    }

    // Compute the iterated dominance frontier of the relevant stores. A node
    // that is both a store and an IDF join has a phi-like merge immediately
    // before the store's GEN/KILL transfer.
    std::vector<bool> idfLocation(cfgSuccessors.size(), false);
    while (!idfWork.empty()) {
      const CFGNode node = idfWork.front();
      idfWork.pop_front();
      for (CFGNode frontier : dominanceFrontiers[node]) {
        if (idfLocation[frontier])
          continue;
        idfLocation[frontier] = true;
        if (!storeLocation[frontier])
          idfWork.push_back(frontier);
      }
    }

    using SparseNode = std::size_t;
    const SparseNode invalidSparse = std::numeric_limits<SparseNode>::max();
    std::vector<std::vector<SparseNode>> sparseSuccessors(1);
    std::vector<std::vector<SparseNode>> sparsePredecessors(1);
    std::vector<AccessID> sparseStore(1, 0);
    std::vector<SparseNode> phiNode(cfgSuccessors.size(), invalidSparse);
    std::vector<SparseNode> storeNode(p.accesses_.size(), invalidSparse);
    auto addSparseNode = [&](AccessID store = 0) {
      const SparseNode node = sparseSuccessors.size();
      sparseSuccessors.emplace_back();
      sparsePredecessors.emplace_back();
      sparseStore.push_back(store);
      return node;
    };
    for (CFGNode location = 0; location < idfLocation.size(); ++location)
      if (idfLocation[location]) {
        phiNode[location] = addSparseNode();
        ++stats.iteratedDominanceFrontierNodes;
      }
    for (AccessID access : sig.accesses)
      if (p.accesses_[access].kind == Program::AccessKind::Store)
        storeNode[access] = addSparseNode(access);

    auto addSparseEdge = [&](SparseNode from, SparseNode to) {
      if (std::find(sparseSuccessors[from].begin(),
                    sparseSuccessors[from].end(),
                    to) != sparseSuccessors[from].end())
        return;
      sparseSuccessors[from].push_back(to);
      sparsePredecessors[to].push_back(from);
      ++stats.sparseGraphEdges;
    };

    // Rename definitions down the dominator tree. IDF nodes receive one input
    // from every incoming control-flow edge; stores consume the current sparse
    // definition and then become the definition seen by following accesses.
    std::vector<SparseNode> definitions{0};
    std::vector<SparseNode> loadDefinition(p.accesses_.size(), invalidSparse);
    struct RenameFrame {
      CFGNode node;
      std::size_t nextChild = 0;
      std::size_t pushes = 0;
      bool entered = false;
    };
    std::vector<RenameFrame> rename{{0}};
    while (!rename.empty()) {
      RenameFrame &frame = rename.back();
      if (!frame.entered) {
        frame.entered = true;
        if (phiNode[frame.node] != invalidSparse) {
          definitions.push_back(phiNode[frame.node]);
          ++frame.pushes;
        }
        const AccessID access = locationAccess[frame.node];
        if (access && sig.accesses.count(access)) {
          if (p.accesses_[access].kind == Program::AccessKind::Store) {
            const SparseNode store = storeNode[access];
            addSparseEdge(definitions.back(), store);
            definitions.push_back(store);
            ++frame.pushes;
          } else {
            loadDefinition[access] = definitions.back();
          }
        }
        for (CFGNode successor : cfgSuccessors[frame.node])
          if (phiNode[successor] != invalidSparse)
            addSparseEdge(definitions.back(), phiNode[successor]);
      }
      if (frame.nextChild < dominatorChildren[frame.node].size()) {
        const CFGNode child = dominatorChildren[frame.node][frame.nextChild++];
        rename.push_back({child});
        continue;
      }
      definitions.resize(definitions.size() - frame.pushes);
      rename.pop_back();
    }

    stats.sparseGraphNodes += sparseSuccessors.size();

    // Solve Algorithm 2's GEN/KILL equations over stores plus IDF joins. Only
    // reaching store-node IDs are carried; no points-to maps flow through the
    // sparse graph.
    std::vector<NodeSet> out(sparseSuccessors.size());
    out[0].insert(p.objects_[object].initialStore);
    std::deque<SparseNode> work;
    std::vector<bool> queued(sparseSuccessors.size(), true);
    for (SparseNode node = 1; node < sparseSuccessors.size(); ++node)
      work.push_back(node);
    while (!work.empty()) {
      const SparseNode node = work.front();
      work.pop_front();
      queued[node] = false;
      NodeSet next;
      for (SparseNode predecessor : sparsePredecessors[node])
        unite(next, out[predecessor]);
      const AccessID store = sparseStore[node];
      if (store) {
        if (sig.strong.count(store))
          next.clear();
        next.insert(p.accesses_[store].value);
      }
      if (next == out[node])
        continue;
      out[node] = std::move(next);
      for (SparseNode successor : sparseSuccessors[node])
        if (!queued[successor]) {
          queued[successor] = true;
          work.push_back(successor);
        }
    }
    for (AccessID access : sig.accesses) {
      if (p.accesses_[access].kind != Program::AccessKind::Load)
        continue;
      const SparseNode source = loadDefinition[access];
      if (source == invalidSparse)
        throw std::logic_error(
            "ValueFlowPTA: relevant load has no sparse definition");
      for (NodeID definition : out[source])
        addIndirect(definition, p.accesses_[access].value);
    }
  }

  void fallback(const char *reason) {
    stats.usedWeakFallback = true;
    stats.fallbackReason = reason;
    // Rebuild, rather than leave edges justified by an invalidated schedule.
    reset(true);
  }

  void run() {
    analyzed = false;
    stats = {};
    reset(!config.enableStrongUpdates);
    while (true) {
      computePointedToBy();
      std::vector<Signature> current(p.objects_.size());
      bool restart = false;
      for (ObjectID object = 1; object < p.objects_.size(); ++object) {
        if (!p.objects_[object].memory)
          continue;
        current[object] = signature(object);
        if (!processed[object] || current[object] == previous[object])
          continue;
        if (!weak) {
          for (AccessID strong : current[object].strong) {
            if (!previous[object].strong.count(strong)) {
              restart = true;
              break;
            }
          }
        }
        pending.insert(object);
      }
      if (restart) {
        fallback("a new strong store invalidated the escape schedule");
        continue;
      }
      if (pending.empty())
        break;

      ObjectID chosen = 0;
      if (weak) {
        chosen = *pending.begin();
      } else {
        for (ObjectID object : pending) {
          const auto escape = escapes(object);
          bool blocked = escape.unresolved;
          for (ObjectID dependency : escape.objects)
            blocked |= pending.count(dependency) != 0;
          if (!blocked) {
            chosen = object;
            break;
          }
        }
        if (!chosen) {
          // Non-singletons cannot kill definitions, so breaking an escape
          // cycle at one of them is safe. Scalar cycles need a fallback.
          for (ObjectID object : pending)
            if (!p.objects_[object].singleton) {
              chosen = object;
              break;
            }
        }
        if (!chosen) {
          fallback(
              "scalar escape cycle or unresolved scalar escape dependency");
          continue;
        }
      }
      pending.erase(chosen);
      processed[chosen] = true;
      previous[chosen] = current[chosen];
      computeIndirect(chosen, current[chosen]);
    }
    for (const auto &sig : previous)
      finalStrong.insert(sig.strong.begin(), sig.strong.end());
    stats.strongUpdateSites = finalStrong.size();
    analyzed = true;
  }
  void requireAnalyzed() const {
    if (!analyzed)
      throw std::logic_error("ValueFlowPTA: analyze() must precede queries");
  }
};

Solver::Solver(const Program &program) : Solver(program, Config{}) {}
Solver::Solver(const Program &program, Config config)
    : impl_(std::make_unique<Impl>(program, config)) {}
Solver::~Solver() = default;
void Solver::analyze() { impl_->run(); }
const PointsToSet &Solver::pointsTo(NodeID node) const {
  impl_->requireAnalyzed();
  impl_->p.checkNode(node);
  return impl_->pts.at(node);
}
const NodeSet &Solver::pointedToBy(ObjectID object) const {
  impl_->requireAnalyzed();
  if (!object || object > impl_->p.objectCount())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  return impl_->pted.at(object);
}
bool Solver::mayAlias(NodeID left, NodeID right) const {
  const auto &a = pointsTo(left);
  const auto &b = pointsTo(right);
  if (a.count(impl_->p.unknownObject()) || b.count(impl_->p.unknownObject()))
    return true;
  for (ObjectID object : a)
    if (object != impl_->p.nullObject() && b.count(object))
      return true;
  return false;
}
bool Solver::isStrongUpdate(AccessID access) const {
  impl_->requireAnalyzed();
  if (!access || access > impl_->p.accessCount())
    throw std::out_of_range("ValueFlowPTA: invalid access");
  return impl_->finalStrong.count(access) != 0;
}
const Solver::Statistics &Solver::statistics() const {
  impl_->requireAnalyzed();
  return impl_->stats;
}

} // namespace lotus::alias::vfg
