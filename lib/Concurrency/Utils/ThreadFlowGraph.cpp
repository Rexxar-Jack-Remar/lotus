/**
 * @file ThreadFlowGraph.cpp
 * @brief Implementation of Thread Flow Graph classes
 *
 * The Thread Flow Graph (TFG) is a graph representation of the concurrent
 * program. Nodes represent synchronization events or instructions. Edges
 * represent:
 * 1. Intra-thread control flow (program order)
 * 2. Inter-thread synchronization (fork, join, signal, etc.)
 */

#include "Concurrency/Utils/ThreadFlowGraph.h"

#include <cassert>
#include <functional>
#include <queue>
#include <unordered_set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace {

bool bfsReachableSameThread(const SyncNode *from, const SyncNode *to) {
  if (!from || !to || from == to) {
    return false;
  }

  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;
  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == to) {
      return true;
    }

    for (SyncNode *succ : current->getSuccessors()) {
      if (succ->getThreadID() != from->getThreadID()) {
        continue;
      }
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

} // namespace

// ============================================================================
// SyncNode Implementation
// ============================================================================

void SyncNode::addPredecessor(SyncNode *pred) {
  if (pred && std::find(m_predecessors.begin(), m_predecessors.end(), pred) ==
                  m_predecessors.end()) {
    m_predecessors.push_back(pred);
  }
}

void SyncNode::addSuccessor(SyncNode *succ) {
  if (succ && std::find(m_successors.begin(), m_successors.end(), succ) ==
                  m_successors.end()) {
    m_successors.push_back(succ);
  }
}

void SyncNode::print(raw_ostream &os) const {
  os << "SyncNode[" << m_node_id << "]: ";
  os << "Type=" << getSyncNodeTypeName(m_type);
  os << ", Thread=" << m_thread_id;
  if (m_call_context_id != 0) {
    os << ", Ctx=" << m_call_context_id;
  }

  if (m_instruction) {
    os << ", Inst=";
    m_instruction->print(os);
  }

  if (m_lock_value) {
    os << ", Lock=";
    m_lock_value->printAsOperand(os, false);
  }

  if (m_forked_thread != 0) {
    os << ", ForkedThread=" << m_forked_thread;
  }

  if (m_joined_thread != 0) {
    os << ", JoinedThread=" << m_joined_thread;
  }
}

std::string SyncNode::toString() const {
  std::string str;
  raw_string_ostream os(str);
  print(os);
  return os.str();
}

// ============================================================================
// ThreadFlowGraph Implementation
// ============================================================================

ThreadFlowGraph::~ThreadFlowGraph() {
  for (auto *node : m_all_nodes) {
    delete node;
  }
}

SyncNode *ThreadFlowGraph::createNode(const Instruction *inst,
                                      SyncNodeType type, ThreadID tid,
                                      CallContextID ctx) {
  if (inst) {
    InstThreadKey key{inst, tid, ctx};
    auto it = m_inst_thread_to_node.find(key);
    if (it != m_inst_thread_to_node.end()) {
      assert(it->second->getType() == type &&
             "conflicting node type for an existing TFG identity");
      return it->second;
    }
  }
  auto *node = new SyncNode(inst, type, tid, ctx, m_next_node_id++);
  m_all_nodes.push_back(node);
  m_thread_nodes[tid].push_back(node);
  invalidateReachabilityIndex();

  if (inst) {
    InstThreadKey key{inst, tid, ctx};
    m_inst_thread_to_node[key] = node;
    m_inst_to_nodes[inst].push_back(node);
  }

  return node;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst, ThreadID tid,
                                   CallContextID ctx) const {
  if (!inst) {
    return nullptr;
  }
  auto it = m_inst_thread_to_node.find({inst, tid, ctx});
  return it != m_inst_thread_to_node.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst,
                                   ThreadID tid) const {
  if (!inst) {
    return nullptr;
  }
  SyncNode *match = nullptr;
  for (SyncNode *node : getNodes(inst, tid)) {
    if (match) {
      return nullptr;
    }
    match = node;
  }
  return match;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst) const {
  auto it = m_inst_to_nodes.find(inst);
  if (it != m_inst_to_nodes.end() && it->second.size() == 1) {
    return it->second.front();
  }
  return nullptr;
}

std::vector<SyncNode *>
ThreadFlowGraph::getNodes(const Instruction *inst) const {
  auto it = m_inst_to_nodes.find(inst);
  if (it == m_inst_to_nodes.end()) {
    return {};
  }
  return it->second;
}

std::vector<SyncNode *> ThreadFlowGraph::getNodes(const Instruction *inst,
                                                  ThreadID tid) const {
  std::vector<SyncNode *> result;
  auto it = m_inst_to_nodes.find(inst);
  if (it == m_inst_to_nodes.end()) {
    return result;
  }
  for (SyncNode *node : it->second) {
    if (node->getThreadID() == tid) {
      result.push_back(node);
    }
  }
  return result;
}

void ThreadFlowGraph::addThread(ThreadID tid, const Function *entry) {
  m_thread_entries[tid] = entry;
}

const Function *ThreadFlowGraph::getThreadEntry(ThreadID tid) const {
  auto it = m_thread_entries.find(tid);
  return it != m_thread_entries.end() ? it->second : nullptr;
}

std::vector<ThreadID> ThreadFlowGraph::getAllThreads() const {
  std::vector<ThreadID> threads;
  threads.reserve(m_thread_entries.size());
  for (const auto &pair : m_thread_entries) {
    threads.push_back(pair.first);
  }
  std::sort(threads.begin(), threads.end());
  return threads;
}

void ThreadFlowGraph::setThreadEntryNode(ThreadID tid, SyncNode *entry) {
  m_thread_entry_nodes[tid] = entry;
}

void ThreadFlowGraph::setThreadExitNode(ThreadID tid, SyncNode *exit) {
  if (!exit) {
    return;
  }
  std::vector<SyncNode *> &exits = m_thread_exit_nodes[tid];
  if (std::find(exits.begin(), exits.end(), exit) == exits.end()) {
    exits.push_back(exit);
  }
}

SyncNode *ThreadFlowGraph::getThreadEntryNode(ThreadID tid) const {
  auto it = m_thread_entry_nodes.find(tid);
  return it != m_thread_entry_nodes.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getThreadExitNode(ThreadID tid) const {
  auto it = m_thread_exit_nodes.find(tid);
  if (it == m_thread_exit_nodes.end() || it->second.size() != 1) {
    return nullptr;
  }
  return it->second.front();
}

std::vector<SyncNode *>
ThreadFlowGraph::getThreadExitNodes(ThreadID tid) const {
  auto it = m_thread_exit_nodes.find(tid);
  return it != m_thread_exit_nodes.end() ? it->second
                                         : std::vector<SyncNode *>();
}

void ThreadFlowGraph::addIntraThreadEdge(SyncNode *from, SyncNode *to) {
  addEdge(from, to, EdgeKind::Control);
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to) {
  addInterThreadEdge(from, to, EdgeKind::Create);
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to,
                                         EdgeKind kind) {
  addEdge(from, to, kind);
}

void ThreadFlowGraph::addCallEdge(SyncNode *call_site, SyncNode *callee_entry) {
  addEdge(call_site, callee_entry, EdgeKind::Call);
}

void ThreadFlowGraph::addRetEdge(SyncNode *callee_exit, SyncNode *return_site) {
  addEdge(callee_exit, return_site, EdgeKind::Ret);
}

namespace {
uint32_t edgeKindBit(EdgeKind kind) {
  return uint32_t{1} << static_cast<unsigned>(kind);
}
} // namespace

void ThreadFlowGraph::addEdge(SyncNode *from, SyncNode *to, EdgeKind kind) {
  if (!from || !to) {
    return;
  }
  from->addSuccessor(to);
  to->addPredecessor(from);
  m_edge_kinds[{from, to}] |= edgeKindBit(kind);
  invalidateReachabilityIndex();
}

std::optional<EdgeKind> ThreadFlowGraph::getEdgeKind(const SyncNode *from,
                                                     const SyncNode *to) const {
  auto it = m_edge_kinds.find({from, to});
  if (it == m_edge_kinds.end()) {
    return std::nullopt;
  }
  for (EdgeKind kind :
       {EdgeKind::Join, EdgeKind::Create, EdgeKind::Barrier,
        EdgeKind::TaskDepend, EdgeKind::TaskWait, EdgeKind::TaskCompletion,
        EdgeKind::Signal, EdgeKind::Call, EdgeKind::Ret, EdgeKind::Control}) {
    if (it->second & edgeKindBit(kind)) {
      return kind;
    }
  }
  return std::nullopt;
}

bool ThreadFlowGraph::hasEdgeKind(const SyncNode *from, const SyncNode *to,
                                  EdgeKind kind) const {
  auto it = m_edge_kinds.find({from, to});
  return it != m_edge_kinds.end() && (it->second & edgeKindBit(kind));
}

void ThreadFlowGraph::setFunctionExitNode(ThreadID tid,
                                          const llvm::Function *func,
                                          SyncNode *exit_node,
                                          CallContextID ctx) {
  if (!exit_node) {
    return;
  }
  std::vector<SyncNode *> &exits = m_func_exit_nodes[{tid, func, ctx}];
  if (std::find(exits.begin(), exits.end(), exit_node) == exits.end()) {
    exits.push_back(exit_node);
  }
}

SyncNode *ThreadFlowGraph::getFunctionExitNode(ThreadID tid,
                                               const llvm::Function *func,
                                               CallContextID ctx) const {
  auto it = m_func_exit_nodes.find({tid, func, ctx});
  if (it == m_func_exit_nodes.end() || it->second.size() != 1) {
    return nullptr;
  }
  return it->second.front();
}

std::vector<SyncNode *>
ThreadFlowGraph::getFunctionExitNodes(ThreadID tid, const llvm::Function *func,
                                      CallContextID ctx) const {
  auto it = m_func_exit_nodes.find({tid, func, ctx});
  return it != m_func_exit_nodes.end() ? it->second : std::vector<SyncNode *>();
}

std::vector<SyncNode *>
ThreadFlowGraph::getNodesOfType(SyncNodeType type) const {
  std::vector<SyncNode *> result;
  for (auto *node : m_all_nodes) {
    if (node->getType() == type) {
      result.push_back(node);
    }
  }
  return result;
}

std::vector<SyncNode *> ThreadFlowGraph::getNodesInThread(ThreadID tid) const {
  auto it = m_thread_nodes.find(tid);
  return it != m_thread_nodes.end() ? it->second : std::vector<SyncNode *>();
}

void ThreadFlowGraph::print(raw_ostream &os) const {
  os << "Thread Flow Graph:\n";
  os << "==================\n";
  os << "Total Nodes: " << m_all_nodes.size() << "\n";
  os << "Total Threads: " << m_thread_entries.size() << "\n\n";

  for (auto *node : m_all_nodes) {
    node->print(os);
    os << "\n";

    if (!node->getSuccessors().empty()) {
      os << "  Successors: ";
      for (auto *succ : node->getSuccessors()) {
        os << succ->getNodeID() << " ";
      }
      os << "\n";
    }
  }
}

void ThreadFlowGraph::printAsDot(raw_ostream &os) const {
  os << "digraph ThreadFlowGraph {\n";
  os << "  rankdir=TB;\n";
  os << "  node [shape=box];\n\n";

  // Define nodes
  for (auto *node : m_all_nodes) {
    os << "  node" << node->getNodeID() << " [label=\"";
    os << "ID:" << node->getNodeID() << "\\n";
    os << "T:" << node->getThreadID() << "\\n";
    os << getSyncNodeTypeName(node->getType());
    os << "\"];\n";
  }

  os << "\n";

  // Define edges
  for (auto *node : m_all_nodes) {
    for (auto *succ : node->getSuccessors()) {
      os << "  node" << node->getNodeID() << " -> node" << succ->getNodeID();

      // Different colors for different edge types
      if (node->getThreadID() != succ->getThreadID()) {
        os << " [color=red, style=dashed]"; // Inter-thread edge
      } else if (isSynchronizationNode(node->getType())) {
        os << " [color=blue]"; // Synchronization edge
      }

      os << ";\n";
    }
  }

  os << "}\n";
}

void ThreadFlowGraph::dumpToFile(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  printAsDot(file);
  file.close();
}

// ============================================================================
// Reachability Index Implementation
// ============================================================================

void ThreadFlowGraph::buildReachabilityIndex() {
  if (m_index_built) {
    return;
  }

  errs() << "Building TFG reachability index...\n";

  for (ThreadID tid : getAllThreads()) {
    buildSCCs(tid);
    buildTopologicalOrder(tid);
  }
  buildReverseEdges();

  m_index_built = true;

  size_t total_indexed = 0;
  for (const auto &entry : m_topo_nodes) {
    total_indexed += entry.second.size();
  }
  errs() << "Indexed " << total_indexed << " nodes across "
         << m_topo_nodes.size() << " threads\n";
}

void ThreadFlowGraph::invalidateReachabilityIndex() {
  m_index_built = false;
  m_topo_order.clear();
  m_topo_nodes.clear();
  m_reverse_edges.clear();
  m_scc_representative.clear();
}

void ThreadFlowGraph::buildReverseEdges() {
  m_reverse_edges.clear();
  for (SyncNode *node : m_all_nodes) {
    for (SyncNode *succ : node->getSuccessors()) {
      m_reverse_edges[succ].push_back(node);
    }
  }
}

void ThreadFlowGraph::buildTopologicalOrder(ThreadID tid) {
  std::vector<SyncNode *> &order = m_topo_nodes[tid];
  order.clear();

  std::vector<SyncNode *> thread_nodes = getNodesInThread(tid);

  if (thread_nodes.empty()) {
    return;
  }

  std::unordered_map<const SyncNode *, size_t> component_index;
  std::vector<std::vector<SyncNode *>> components;
  for (SyncNode *node : thread_nodes) {
    const SyncNode *representative = m_scc_representative[node];
    auto insertion = component_index.emplace(representative, components.size());
    if (insertion.second) {
      components.emplace_back();
    }
    components[insertion.first->second].push_back(node);
  }

  std::vector<std::vector<size_t>> successors(components.size());
  std::vector<size_t> in_degree(components.size(), 0);
  std::set<std::pair<size_t, size_t>> seen_edges;
  for (SyncNode *node : thread_nodes) {
    size_t from_component = component_index[m_scc_representative[node]];
    for (SyncNode *succ : node->getSuccessors()) {
      if (succ->getThreadID() != tid) {
        continue;
      }
      size_t to_component = component_index[m_scc_representative[succ]];
      if (from_component == to_component ||
          !seen_edges.insert({from_component, to_component}).second) {
        continue;
      }
      successors[from_component].push_back(to_component);
      ++in_degree[to_component];
    }
  }

  std::queue<size_t> ready;
  for (size_t component = 0; component < components.size(); ++component) {
    if (in_degree[component] == 0) {
      ready.push(component);
    }
  }

  int rank = 0;
  while (!ready.empty()) {
    size_t component = ready.front();
    ready.pop();
    std::sort(components[component].begin(), components[component].end(),
              [](const SyncNode *lhs, const SyncNode *rhs) {
                return lhs->getNodeID() < rhs->getNodeID();
              });
    for (SyncNode *node : components[component]) {
      m_topo_order[node->getNodeID()] = rank;
      order.push_back(node);
    }
    ++rank;
    for (size_t successor : successors[component]) {
      if (--in_degree[successor] == 0) {
        ready.push(successor);
      }
    }
  }
}

void ThreadFlowGraph::buildSCCs(ThreadID tid) {
  std::vector<SyncNode *> thread_nodes = getNodesInThread(tid);
  std::unordered_set<const SyncNode *> visited;
  std::vector<SyncNode *> finish_order;
  finish_order.reserve(thread_nodes.size());

  struct DFSFrame {
    SyncNode *node;
    size_t next_successor = 0;
  };

  for (SyncNode *root : thread_nodes) {
    if (!visited.insert(root).second) {
      continue;
    }
    std::vector<DFSFrame> stack{{root, 0}};
    while (!stack.empty()) {
      DFSFrame &frame = stack.back();
      const auto &successors = frame.node->getSuccessors();
      bool descended = false;
      while (frame.next_successor < successors.size()) {
        SyncNode *succ = successors[frame.next_successor++];
        if (succ->getThreadID() == tid && visited.insert(succ).second) {
          stack.push_back({succ, 0});
          descended = true;
          break;
        }
      }
      if (!descended && frame.next_successor >= successors.size()) {
        finish_order.push_back(frame.node);
        stack.pop_back();
      }
    }
  }

  std::unordered_map<SyncNode *, std::vector<SyncNode *>> reverse;
  for (SyncNode *node : thread_nodes) {
    for (SyncNode *succ : node->getSuccessors()) {
      if (succ->getThreadID() == tid) {
        reverse[succ].push_back(node);
      }
    }
  }

  visited.clear();
  for (auto it = finish_order.rbegin(); it != finish_order.rend(); ++it) {
    SyncNode *representative = *it;
    if (!visited.insert(representative).second) {
      continue;
    }
    std::vector<SyncNode *> stack{representative};
    while (!stack.empty()) {
      SyncNode *node = stack.back();
      stack.pop_back();
      m_scc_representative[node] = representative;
      for (SyncNode *pred : reverse[node]) {
        if (visited.insert(pred).second) {
          stack.push_back(pred);
        }
      }
    }
  }
}

bool ThreadFlowGraph::canReach(const SyncNode *from, const SyncNode *to) const {
  if (!from || !to || from == to) {
    return false;
  }

  if (m_index_built && from->getThreadID() == to->getThreadID()) {
    return canReachViaIndex(from, to);
  }

  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;

  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == to) {
      return true;
    }

    for (SyncNode *succ : current->getSuccessors()) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool ThreadFlowGraph::canReachViaIndex(const SyncNode *from,
                                       const SyncNode *to) const {
  auto from_it = m_topo_order.find(from->getNodeID());
  auto to_it = m_topo_order.find(to->getNodeID());

  if (from_it == m_topo_order.end() || to_it == m_topo_order.end()) {
    return bfsReachableSameThread(from, to);
  }

  if (from_it->second > to_it->second) {
    auto from_rep = m_scc_representative.find(from);
    auto to_rep = m_scc_representative.find(to);

    if (from_rep != m_scc_representative.end() &&
        to_rep != m_scc_representative.end() &&
        from_rep->second == to_rep->second) {
      return bfsReachableSameThread(from, to);
    }

    return false;
  }

  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;

  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == to) {
      return true;
    }

    auto current_it = m_topo_order.find(current->getNodeID());
    if (current_it != m_topo_order.end() &&
        current_it->second > to_it->second) {
      continue;
    }

    for (SyncNode *succ : current->getSuccessors()) {
      if (succ->getThreadID() == from->getThreadID()) {
        if (visited.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  }

  return false;
}

int ThreadFlowGraph::getTopologicalOrder(const SyncNode *node) const {
  auto it = m_topo_order.find(node->getNodeID());
  return it != m_topo_order.end() ? it->second : -1;
}

const std::vector<SyncNode *> &
ThreadFlowGraph::getTopologicalOrderNodes(ThreadID tid) const {
  static const std::vector<SyncNode *> empty;
  auto it = m_topo_nodes.find(tid);
  return it != m_topo_nodes.end() ? it->second : empty;
}

// ============================================================================
// Utility Functions
// ============================================================================

namespace mhp {

StringRef getSyncNodeTypeName(SyncNodeType type) {
  switch (type) {
  case SyncNodeType::THREAD_START:
    return "THREAD_START";
  case SyncNodeType::THREAD_FORK:
    return "THREAD_FORK";
  case SyncNodeType::THREAD_JOIN:
    return "THREAD_JOIN";
  case SyncNodeType::THREAD_EXIT:
    return "THREAD_EXIT";
  case SyncNodeType::LOCK_ACQUIRE:
    return "LOCK_ACQUIRE";
  case SyncNodeType::LOCK_RELEASE:
    return "LOCK_RELEASE";
  case SyncNodeType::COND_WAIT:
    return "COND_WAIT";
  case SyncNodeType::COND_SIGNAL:
    return "COND_SIGNAL";
  case SyncNodeType::COND_BROADCAST:
    return "COND_BROADCAST";
  case SyncNodeType::BARRIER_WAIT:
    return "BARRIER_WAIT";
  case SyncNodeType::REGULAR_INST:
    return "REGULAR_INST";
  case SyncNodeType::FUNCTION_CALL:
    return "FUNCTION_CALL";
  case SyncNodeType::FUNCTION_RETURN:
    return "FUNCTION_RETURN";
  }
  return "UNKNOWN";
}

bool isSynchronizationNode(SyncNodeType type) {
  return type == SyncNodeType::LOCK_ACQUIRE ||
         type == SyncNodeType::LOCK_RELEASE ||
         type == SyncNodeType::COND_WAIT || type == SyncNodeType::COND_SIGNAL ||
         type == SyncNodeType::COND_BROADCAST ||
         type == SyncNodeType::BARRIER_WAIT;
}

bool isThreadBoundaryNode(SyncNodeType type) {
  return type == SyncNodeType::THREAD_START ||
         type == SyncNodeType::THREAD_FORK ||
         type == SyncNodeType::THREAD_JOIN || type == SyncNodeType::THREAD_EXIT;
}

} // namespace mhp
