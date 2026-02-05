/**
 * @file ThreadFlowGraph.cpp
 * @brief Implementation of Thread Flow Graph classes
 *
 * The Thread Flow Graph (TFG) is a graph representation of the concurrent program.
 * Nodes represent synchronization events or instructions.
 * Edges represent:
 * 1. Intra-thread control flow (program order)
 * 2. Inter-thread synchronization (fork, join, signal, etc.)
 */

#include "Analysis/Concurrency/ThreadFlowGraph.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

// ============================================================================
// SyncNode Implementation
// ============================================================================

size_t SyncNode::next_id = 0;

void SyncNode::print(raw_ostream &os) const {
  os << "SyncNode[" << m_node_id << "]: ";
  os << "Type=" << getSyncNodeTypeName(m_type);
  os << ", Thread=" << m_thread_id;

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
                                      SyncNodeType type, ThreadID tid) {
  if (inst) {
    auto it = m_inst_to_node.find(inst);
    if (it != m_inst_to_node.end()) {
      return it->second;
    }
  }
  auto *node = new SyncNode(inst, type, tid);
  m_all_nodes.push_back(node);

  if (inst) {
    m_inst_to_node[inst] = node;
  }

  return node;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst) const {
  auto it = m_inst_to_node.find(inst);
  return it != m_inst_to_node.end() ? it->second : nullptr;
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
  return threads;
}

void ThreadFlowGraph::setThreadEntryNode(ThreadID tid, SyncNode *entry) {
  m_thread_entry_nodes[tid] = entry;
}

void ThreadFlowGraph::setThreadExitNode(ThreadID tid, SyncNode *exit) {
  m_thread_exit_nodes[tid] = exit;
}

SyncNode *ThreadFlowGraph::getThreadEntryNode(ThreadID tid) const {
  auto it = m_thread_entry_nodes.find(tid);
  return it != m_thread_entry_nodes.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getThreadExitNode(ThreadID tid) const {
  auto it = m_thread_exit_nodes.find(tid);
  return it != m_thread_exit_nodes.end() ? it->second : nullptr;
}

void ThreadFlowGraph::addIntraThreadEdge(SyncNode *from, SyncNode *to) {
  if (from && to) {
    from->addSuccessor(to);
    to->addPredecessor(from);
    m_edge_kinds[{const_cast<const SyncNode *>(from), const_cast<const SyncNode *>(to)}] = EdgeKind::Control;
  }
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to) {
  addInterThreadEdge(from, to, EdgeKind::Create);
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to,
                                         EdgeKind kind) {
  if (from && to) {
    from->addSuccessor(to);
    to->addPredecessor(from);
    m_edge_kinds[{const_cast<const SyncNode *>(from), const_cast<const SyncNode *>(to)}] = kind;
  }
}

void ThreadFlowGraph::addCallEdge(SyncNode *call_site, SyncNode *callee_entry) {
  if (call_site && callee_entry) {
    call_site->addSuccessor(callee_entry);
    callee_entry->addPredecessor(call_site);
    m_edge_kinds[{const_cast<const SyncNode *>(call_site), const_cast<const SyncNode *>(callee_entry)}] = EdgeKind::Call;
  }
}

void ThreadFlowGraph::addRetEdge(SyncNode *callee_exit, SyncNode *return_site) {
  if (callee_exit && return_site) {
    callee_exit->addSuccessor(return_site);
    return_site->addPredecessor(callee_exit);
    m_edge_kinds[{const_cast<const SyncNode *>(callee_exit), const_cast<const SyncNode *>(return_site)}] = EdgeKind::Ret;
  }
}

EdgeKind ThreadFlowGraph::getEdgeKind(const SyncNode *from, const SyncNode *to) const {
  auto it = m_edge_kinds.find({from, to});
  return it != m_edge_kinds.end() ? it->second : EdgeKind::Control;
}

void ThreadFlowGraph::setFunctionExitNode(ThreadID tid,
                                          const llvm::Function *func,
                                          SyncNode *exit_node) {
  m_func_exit_nodes[{tid, func}] = exit_node;
}

SyncNode *ThreadFlowGraph::getFunctionExitNode(
    ThreadID tid, const llvm::Function *func) const {
  auto it = m_func_exit_nodes.find({tid, func});
  return it != m_func_exit_nodes.end() ? it->second : nullptr;
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

std::vector<SyncNode *>
ThreadFlowGraph::getNodesInThread(ThreadID tid) const {
  std::vector<SyncNode *> result;
  for (auto *node : m_all_nodes) {
    if (node->getThreadID() == tid) {
      result.push_back(node);
    }
  }
  return result;
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
         type == SyncNodeType::COND_WAIT ||
         type == SyncNodeType::COND_SIGNAL ||
         type == SyncNodeType::COND_BROADCAST ||
         type == SyncNodeType::BARRIER_WAIT;
}

bool isThreadBoundaryNode(SyncNodeType type) {
  return type == SyncNodeType::THREAD_START ||
         type == SyncNodeType::THREAD_FORK ||
         type == SyncNodeType::THREAD_JOIN ||
         type == SyncNodeType::THREAD_EXIT;
}

} // namespace mhp
