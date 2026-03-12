/**
 * @file StaticVectorClockMHP.cpp
 * @brief Implementation of the static vector clock based MHP analysis (CGO 1).
 */

#include "Analysis/Concurrency/MHP/StaticVectorClockMHP.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

#include <deque>
#include <set>
#include <unordered_set>

using namespace llvm;
using namespace mhp;

StaticVectorClockMHP::StaticVectorClockMHP(Module &module)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()) {
  m_tfg = std::make_unique<ThreadFlowGraph>();
}

void StaticVectorClockMHP::analyze() {
  buildThreadFlowGraph();
  buildStaticThreads();
  computeReachabilityPerStaticThread();
  initializeNodeClocks();
  computeStaticVectorClocks();
  computeMHPPairs();
}

// === StaticVectorClock helpers =============================================

bool StaticVectorClockMHP::StaticVectorClock::mergeFrom(
    const StaticVectorClock &other) {
  bool changed = false;
  for (const auto &kv : other.entries) {
    auto &dest = entries[kv.first];
    for (const auto &elem : kv.second) {
      changed |= dest.insert(elem).second;
    }
  }
  return changed;
}

bool StaticVectorClockMHP::StaticVectorClock::leq(
    const StaticVectorClock &other) const {
  for (const auto &kv : entries) {
    auto it = other.entries.find(kv.first);
    if (it == other.entries.end()) {
      if (!kv.second.empty())
        return false;
      continue;
    }
    const auto &rhs = it->second;
    for (const auto &elem : kv.second) {
      if (rhs.find(elem) == rhs.end())
        return false;
    }
  }
  return true;
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::initialClockFor(const StaticThread &st) const {
  StaticVectorClock init;
  init.entries[st.id].insert({LogicClockElem::Kind::Start, 0});
  // Paper: SV = {T_main → {S}, T → {⊤}} for others. ⊤ = not yet started.
  LogicClockElem::Kind otherKind = LogicClockElem::Kind::Top;
  for (const auto &s : m_static_threads) {
    if (s.id != st.id)
      init.entries[s.id].insert({otherKind, 0});
  }
  return init;
}

StaticThreadID StaticVectorClockMHP::getOrCreateStaticThread(const Context &ctx,
                                                             ThreadID base_tid,
                                                             const SyncNode *entry) {
  auto it = m_ctx_to_stid.find(ctx);
  if (it != m_ctx_to_stid.end())
    return it->second;

  StaticThreadID new_id = m_static_threads.size();
  StaticThread st;
  st.id = new_id;
  st.ctx = ctx;
  st.base_tid = base_tid;
  st.entry = entry;
  m_static_threads.push_back(st);
  m_ctx_to_stid[ctx] = new_id;
  return new_id;
}

void StaticVectorClockMHP::buildStaticThreads() {
  m_static_threads.clear();
  m_ctx_to_stid.clear();
  m_node_to_static_thread.clear();
  m_inst_to_static_thread.clear();
  m_node_clocks.clear();
  m_node_id_to_node.clear();

  if (!m_tfg)
    return;

  const SyncNode *main_entry = m_tfg->getThreadEntryNode(0);
  Context root;
  root.call_sites.clear();
  root.fork_sites.clear();
  StaticThreadID root_id = getOrCreateStaticThread(root, 0, main_entry);

  using WorkItem = std::tuple<StaticThreadID, const SyncNode *, std::vector<size_t>>;
  std::deque<WorkItem> worklist;
  worklist.push_back({root_id, main_entry, {}});

  std::set<std::pair<StaticThreadID, const SyncNode *>> visited;
  while (!worklist.empty()) {
    StaticThreadID stid;
    const SyncNode *node;
    std::vector<size_t> call_sites;
    std::tie(stid, node, call_sites) = worklist.front();
    worklist.pop_front();

    if (!node || !visited.insert({stid, node}).second)
      continue;

    StaticThread &st = m_static_threads[stid];
    st.nodes.push_back(node);
    m_node_to_static_thread[node] = stid;
    m_node_id_to_node[node->getNodeID()] = node;
    if (const Instruction *inst = node->getInstruction()) {
      m_inst_to_static_thread[inst] = stid;
    }

    for (SyncNode *succ : node->getSuccessors()) {
      EdgeKind kind = m_tfg->getEdgeKind(node, succ);
      std::vector<size_t> new_call_sites = call_sites;
      if (kind == EdgeKind::Call) {
        if (new_call_sites.size() >= kCallContextLimit)
          new_call_sites.erase(new_call_sites.begin());
        new_call_sites.push_back(node->getNodeID());
      } else if (kind == EdgeKind::Ret) {
        if (!new_call_sites.empty())
          new_call_sites.pop_back();
      }
      if (succ->getThreadID() == st.base_tid) {
        worklist.push_back({stid, succ, new_call_sites});
      } else if (kind == EdgeKind::Create) {
        Context child_ctx;
        child_ctx.call_sites = call_sites;
        child_ctx.fork_sites = st.ctx.fork_sites;
        child_ctx.fork_sites.push_back(node->getNodeID());
        StaticThreadID child_stid =
            getOrCreateStaticThread(child_ctx, succ->getThreadID(), succ);
        worklist.push_back({child_stid, succ, {}});
      } else if (kind == EdgeKind::Join || kind == EdgeKind::Signal ||
                 kind == EdgeKind::Barrier) {
        ThreadID succ_tid = succ->getThreadID();
        StaticThreadID succ_stid = 0;
        if (m_node_to_static_thread.count(succ)) {
          succ_stid = m_node_to_static_thread[succ];
        } else {
          for (const auto &s : m_static_threads) {
            if (s.base_tid == succ_tid) {
              succ_stid = s.id;
              break;
            }
          }
        }
        worklist.push_back({succ_stid, succ, {}});
      }
    }
  }
}

void StaticVectorClockMHP::initializeNodeClocks() {
  for (const auto &kv : m_node_to_static_thread) {
    const SyncNode *node = kv.first;
    StaticThreadID stid = kv.second;
    if (stid < m_static_threads.size()) {
      m_node_clocks[node] = initialClockFor(m_static_threads[stid]);
    }
  }
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::mergePredecessorClocks(const SyncNode *node) const {
  StaticVectorClock merged;
  if (!node)
    return merged;

  for (auto *pred : node->getPredecessors()) {
    auto it = m_node_clocks.find(pred);
    if (it != m_node_clocks.end()) {
      merged.mergeFrom(it->second);
    }
  }
  return merged;
}

bool StaticVectorClockMHP::logicClockLeq(const LogicClockElem &a,
                                         const LogicClockElem &b,
                                         StaticThreadID stid) const {
  // Paper partial order: ⊤ ≤ S ≤ n@c ≤ ⊥ (⊤ min, ⊥ max). leq(a,b) = true iff a ≤ b.
  using K = LogicClockElem::Kind;
  if (a.kind == K::Top)
    return true;  // ⊤ ≤ anything
  if (b.kind == K::Terminated)
    return true;  // anything ≤ ⊥
  if (a.kind == K::Terminated)
    return (b.kind == K::Terminated);  // ⊥ ≤ only ⊥
  if (b.kind == K::Top)
    return (a.kind == K::Top);  // a ≤ ⊤ only when a = ⊤
  if (a.kind == K::Start && b.kind == K::Start)
    return true;
  if (a.kind == K::Start && b.kind == K::Node)
    return true;
  if (a.kind == K::Node && b.kind == K::Terminated)
    return true;
  if (a.kind == K::Node && b.kind == K::Node) {
    auto it_a = m_node_id_to_node.find(a.node_id);
    auto it_b = m_node_id_to_node.find(b.node_id);
    if (it_a == m_node_id_to_node.end() || it_b == m_node_id_to_node.end())
      return false;
    return nodeReachesInStaticThread(it_a->second, it_b->second, stid);
  }
  return false;
}

void StaticVectorClockMHP::logicClockMax(const LogicClockSet &la,
                                         const LogicClockSet &lb,
                                         StaticThreadID stid,
                                         LogicClockSet &out) const {
  for (const auto &a : la) {
    bool dominated = false;
    for (const auto &b : lb) {
      if (logicClockLeq(a, b, stid)) {
        dominated = true;
        break;
      }
    }
    if (!dominated)
      out.insert(a);
  }
  for (const auto &b : lb) {
    bool dominated = false;
    for (const auto &a : la) {
      if (logicClockLeq(b, a, stid)) {
        dominated = true;
        break;
      }
    }
    if (!dominated)
      out.insert(b);
  }
}

bool StaticVectorClockMHP::nodeReachesInStaticThread(const SyncNode *from,
                                                     const SyncNode *to,
                                                     StaticThreadID stid) const {
  if (from == to)
    return true;
  auto it = m_reachable_from_cs.find(stid);
  if (it == m_reachable_from_cs.end())
    return false;
  auto it2 = it->second.find(from);
  if (it2 == it->second.end())
    return false;
  for (const auto &kv : it2->second) {
    if (kv.second.count(to))
      return true;
  }
  return false;
}

void StaticVectorClockMHP::computeReachabilityPerStaticThread() {
  m_reachable_from_cs.clear();
  if (!m_tfg)
    return;

  using NodeCtx = std::pair<const SyncNode *, CallString>;
  struct NodeCtxHash {
    size_t operator()(const NodeCtx &nc) const {
      return std::hash<const SyncNode *>()(nc.first) * 31 + CallStringHash()(nc.second);
    }
  };
  struct NodeCtxEq {
    bool operator()(const NodeCtx &a, const NodeCtx &b) const {
      return a.first == b.first && a.second == b.second;
    }
  };

  for (const auto &st : m_static_threads) {
    std::unordered_set<const SyncNode *> in_thread(st.nodes.begin(), st.nodes.end());
    if (!st.entry || !in_thread.count(st.entry))
      continue;

    // Phase 1: discover all (node, ctx) reachable from entry via context-sensitive BFS
    std::unordered_set<NodeCtx, NodeCtxHash, NodeCtxEq> discovered;
    std::deque<NodeCtx> worklist;
    worklist.push_back({st.entry, {}});
    discovered.insert({st.entry, {}});

    while (!worklist.empty()) {
      const SyncNode *n = worklist.front().first;
      CallString ctx = worklist.front().second;
      worklist.pop_front();

      for (SyncNode *succ : n->getSuccessors()) {
        if (!in_thread.count(succ))
          continue;
        EdgeKind kind = m_tfg->getEdgeKind(n, succ);
        CallString new_ctx;
        if (kind == EdgeKind::Control) {
          new_ctx = ctx;
        } else if (kind == EdgeKind::Call) {
          new_ctx = ctx;
          if (new_ctx.size() >= kCallContextLimit)
            new_ctx.erase(new_ctx.begin());
          new_ctx.push_back(n->getNodeID());
        } else if (kind == EdgeKind::Ret) {
          auto rit = m_ret_to_call.find(succ);
          if (rit == m_ret_to_call.end() || ctx.empty())
            continue;
          if (ctx.back() != rit->second->getNodeID())
            continue;
          new_ctx = ctx;
          new_ctx.pop_back();
        } else {
          continue;  // Create, Join, Signal, Barrier: stay in thread, treat as Control
        }

        NodeCtx nc = {succ, new_ctx};
        if (discovered.insert(nc).second)
          worklist.push_back(nc);
      }
    }

    // Phase 2: for each (node, ctx), compute reachable nodes via context-sensitive BFS
    for (const NodeCtx &start_nc : discovered) {
      const SyncNode *start = start_nc.first;
      const CallString &start_ctx = start_nc.second;
      std::unordered_set<const SyncNode *> &reached =
          m_reachable_from_cs[st.id][start][start_ctx];

      std::deque<NodeCtx> q;
      std::unordered_set<NodeCtx, NodeCtxHash, NodeCtxEq> visited;
      q.push_back(start_nc);
      visited.insert(start_nc);
      reached.insert(start);

      while (!q.empty()) {
        const SyncNode *n = q.front().first;
        CallString ctx = q.front().second;
        q.pop_front();

        for (SyncNode *succ : n->getSuccessors()) {
          if (!in_thread.count(succ))
            continue;
          EdgeKind kind = m_tfg->getEdgeKind(n, succ);
          CallString new_ctx;
          if (kind == EdgeKind::Control) {
            new_ctx = ctx;
          } else if (kind == EdgeKind::Call) {
            new_ctx = ctx;
            if (new_ctx.size() >= kCallContextLimit)
              new_ctx.erase(new_ctx.begin());
            new_ctx.push_back(n->getNodeID());
          } else if (kind == EdgeKind::Ret) {
            auto rit = m_ret_to_call.find(succ);
            if (rit == m_ret_to_call.end() || ctx.empty())
              continue;
            if (ctx.back() != rit->second->getNodeID())
              continue;
            new_ctx = ctx;
            new_ctx.pop_back();
          } else {
            continue;
          }

          NodeCtx nc = {succ, new_ctx};
          reached.insert(succ);
          if (visited.insert(nc).second)
            q.push_back(nc);
        }
      }
    }
  }
}

bool StaticVectorClockMHP::svcLeq(const StaticVectorClock &lhs,
                                  const StaticVectorClock &rhs) const {
  for (const auto &kv : lhs.entries) {
    StaticThreadID stid = kv.first;
    auto it = rhs.entries.find(stid);
    const LogicClockSet &rhs_set = (it != rhs.entries.end()) ? it->second : LogicClockSet();
    for (const auto &l1 : kv.second) {
      bool found = false;
      for (const auto &l2 : rhs_set) {
        if (logicClockLeq(l1, l2, stid)) {
          found = true;
          break;
        }
      }
      if (!found && !rhs_set.empty())
        return false;
      if (!found && rhs_set.empty() && l1.kind != LogicClockElem::Kind::Terminated)
        return false;
    }
  }
  return true;
}

void StaticVectorClockMHP::computeSVMax(const StaticVectorClock &sv1,
                                         const StaticVectorClock &sv2,
                                         StaticVectorClock &out) const {
  out.entries.clear();
  for (const auto &kv : sv1.entries) {
    StaticThreadID stid = kv.first;
    auto it = sv2.entries.find(stid);
    if (it == sv2.entries.end()) {
      out.entries[stid] = kv.second;
      continue;
    }
    logicClockMax(kv.second, it->second, stid, out.entries[stid]);
  }
  for (const auto &kv : sv2.entries) {
    if (sv1.entries.count(kv.first))
      continue;
    out.entries[kv.first] = kv.second;
  }
}

std::unordered_set<ThreadID>
StaticVectorClockMHP::getDescendantThreadIDs(ThreadID tid) const {
  std::unordered_set<ThreadID> result;
  std::deque<ThreadID> worklist;
  auto it = m_thread_children.find(tid);
  if (it != m_thread_children.end()) {
    for (ThreadID c : it->second)
      worklist.push_back(c);
  }
  while (!worklist.empty()) {
    ThreadID cur = worklist.front();
    worklist.pop_front();
    if (!result.insert(cur).second)
      continue;
    auto cit = m_thread_children.find(cur);
    if (cit != m_thread_children.end()) {
      for (ThreadID c : cit->second)
        worklist.push_back(c);
    }
  }
  return result;
}

void StaticVectorClockMHP::addEventToClock(const SyncNode *node,
                                           StaticVectorClock &sv) const {
  auto st_it = m_node_to_static_thread.find(node);
  if (st_it == m_node_to_static_thread.end())
    return;

  StaticThreadID stid = st_it->second;
  sv.entries[stid].insert({LogicClockElem::Kind::Node, node->getNodeID()});

  if (node->getType() == SyncNodeType::THREAD_EXIT) {
    sv.entries[stid].insert({LogicClockElem::Kind::Terminated, 0});
  }
  // Paper Figure 4 [CREATE]: at fork set T(n2) and all descendant T' to {S}.
  if (node->getType() == SyncNodeType::THREAD_FORK) {
    ThreadID child_tid = node->getForkedThread();
    std::unordered_set<ThreadID> desc_tids = getDescendantThreadIDs(child_tid);
    desc_tids.insert(child_tid);
    for (const auto &st : m_static_threads) {
      if (!desc_tids.count(st.base_tid))
        continue;
      // Direct child: only the static thread created at this fork site.
      // Descendants: all static threads with that base_tid.
      bool isDirectChild = (st.base_tid == child_tid);
      bool matchesFork =
          !st.ctx.fork_sites.empty() &&
          st.ctx.fork_sites.back() == node->getNodeID();
      if ((isDirectChild && matchesFork) || (!isDirectChild)) {
        sv.entries[st.id].clear();
        sv.entries[st.id].insert({LogicClockElem::Kind::Start, 0});
      }
    }
  }
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::mergePredecessorClocksWithRules(const SyncNode *node) const {
  StaticVectorClock merged;
  if (!node)
    return merged;

  // Paper [JOIN]/[SIGNAL]: SVn2@c = SVMax(SVn1@Ø, SVn2@c)[...]. We must first
  // merge all intra-thread predecessors (⊓), then apply SVMax with join/signal.
  for (SyncNode *pred : node->getPredecessors()) {
    auto it = m_node_clocks.find(pred);
    if (it == m_node_clocks.end())
      continue;
    EdgeKind kind = m_tfg->getEdgeKind(pred, node);
    if (kind == EdgeKind::Join || kind == EdgeKind::Signal ||
        kind == EdgeKind::Barrier)
      continue;
    merged.mergeFrom(it->second);
  }
  for (SyncNode *pred : node->getPredecessors()) {
    auto it = m_node_clocks.find(pred);
    if (it == m_node_clocks.end())
      continue;
    EdgeKind kind = m_tfg->getEdgeKind(pred, node);
    if (kind != EdgeKind::Join && kind != EdgeKind::Signal &&
        kind != EdgeKind::Barrier)
      continue;
    StaticVectorClock maxClock;
    computeSVMax(merged, it->second, maxClock);
    merged = std::move(maxClock);
  }
  return merged;
}

bool StaticVectorClockMHP::shouldAddEventAtNode(const SyncNode *node) const {
  // Paper Figure 4: [CALL] SV_n2@c' = SV_n1@c (no add); [RET] SV_n2@c = SV_n1@c' (no add);
  // [CREATE] at child entry SV_n2@Ø = SV_before[...] (no add). Only [DEFAULT], [JOIN], [SIGNAL] add the current event.
  if (!node || !m_tfg)
    return true;
  for (SyncNode *pred : node->getPredecessors()) {
    EdgeKind kind = m_tfg->getEdgeKind(pred, node);
    if (kind == EdgeKind::Control || kind == EdgeKind::Join ||
        kind == EdgeKind::Signal || kind == EdgeKind::Barrier)
      return true;
  }
  return false;
}

bool StaticVectorClockMHP::transfer(const SyncNode *node) {
  if (!node)
    return false;

  // Figure 4: merge/Max from predecessors; then add current event only when not Call/Ret/Create target.
  StaticVectorClock incoming = mergePredecessorClocksWithRules(node);

  if (incoming.entries.empty()) {
    auto st_it = m_node_to_static_thread.find(node);
    if (st_it != m_node_to_static_thread.end()) {
      incoming = initialClockFor(m_static_threads[st_it->second]);
    }
  }

  if (shouldAddEventAtNode(node))
    addEventToClock(node, incoming);

  StaticVectorClock &current = m_node_clocks[node];
  bool changed = !incoming.leq(current) || !current.leq(incoming);
  if (changed) {
    current = incoming;
  }
  return changed;
}

void StaticVectorClockMHP::computeStaticVectorClocks() {
  if (!m_tfg)
    return;

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &pair : m_node_clocks) {
      const SyncNode *node = pair.first;
      changed |= transfer(node);
    }
  }
}

bool StaticVectorClockMHP::happensBefore(const StaticVectorClock &lhs,
                                         const StaticVectorClock &rhs) const {
  // Returns true if lhs happens-before rhs (strictly ordered).
  // Paper: use pointwise logic-clock order (svcLeq), not set inclusion.
  return svcLeq(lhs, rhs) && !svcLeq(rhs, lhs);
}

bool StaticVectorClockMHP::happensBefore(const Instruction *i1,
                                         const Instruction *i2) const {
  if (!m_tfg)
    return false;

  if (!i1 || !i2 || i1 == i2)
    return false;

  if (isInstructionThreadAmbiguous(i1) || isInstructionThreadAmbiguous(i2))
    return false;

  auto tid1 = m_inst_to_thread.find(i1);
  auto tid2 = m_inst_to_thread.find(i2);
  if (tid1 == m_inst_to_thread.end() || tid2 == m_inst_to_thread.end()) {
    return false;
  }

  std::vector<SyncNode *> n1s = m_tfg->getNodes(i1, tid1->second);
  std::vector<SyncNode *> n2s = m_tfg->getNodes(i2, tid2->second);
  if (n1s.empty() || n2s.empty())
    return false;

  for (const SyncNode *n1 : n1s) {
    auto it1 = m_node_clocks.find(n1);
    if (it1 == m_node_clocks.end()) {
      return false;
    }
    for (const SyncNode *n2 : n2s) {
      auto it2 = m_node_clocks.find(n2);
      if (it2 == m_node_clocks.end()) {
        return false;
      }
      if (!(svcLeq(it1->second, it2->second) &&
            !svcLeq(it2->second, it1->second))) {
        return false;
      }
    }
  }

  return true;
}

void StaticVectorClockMHP::computeMHPPairs() {
  std::vector<const Instruction *> all_insts;
  all_insts.reserve(m_inst_to_thread.size());
  for (Function &func : m_module) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_inst_to_thread.count(inst)) {
        all_insts.push_back(inst);
      }
    }
  }

  for (size_t i = 0; i < all_insts.size(); ++i) {
    for (size_t j = i + 1; j < all_insts.size(); ++j) {
      const Instruction *a = all_insts[i];
      const Instruction *b = all_insts[j];

      if (happensBefore(a, b) || happensBefore(b, a)) {
        continue;
      }

      m_mhp_pairs.insert({a, b});
    }
  }
}

bool StaticVectorClockMHP::mayHappenInParallel(const Instruction *i1,
                                               const Instruction *i2) const {
  if (!i1 || !i2 || i1 == i2)
    return false;

  if (m_mhp_pairs.count({i1, i2}) || m_mhp_pairs.count({i2, i1}))
    return true;

  return !happensBefore(i1, i2) && !happensBefore(i2, i1);
}

void StaticVectorClockMHP::printStatistics(raw_ostream &os) const {
  size_t num_static_threads = m_static_threads.size();
  size_t num_nodes = m_node_clocks.size();

  os << "SVC-MHP Statistics:\n";
  os << "===================\n";
  os << "Static Threads: " << num_static_threads << "\n";
  os << "TFG Nodes:      " << num_nodes << "\n";
  os << "MHP Pairs:      " << m_mhp_pairs.size() << "\n";
}

void StaticVectorClockMHP::printResults(raw_ostream &os) const {
  printStatistics(os);
  os << "\nSample MHP pairs (up to 10):\n";
  int shown = 0;
  for (const auto &p : m_mhp_pairs) {
    os << "MHP: ";
    p.first->print(os);
    os << " || ";
    p.second->print(os);
    os << "\n";
    if (++shown >= 10) {
      if (m_mhp_pairs.size() > 10) {
        os << "... (" << (m_mhp_pairs.size() - 10) << " more)\n";
      }
      break;
    }
  }
}

// === Thread-flow graph construction (self contained, no MHPAnalysis) ========

void StaticVectorClockMHP::buildThreadFlowGraph() {
  m_tfg = std::make_unique<ThreadFlowGraph>();
  m_call_graph = std::make_unique<CallGraph>(m_module);

  Function *main_func = m_module.getFunction("main");
  if (!main_func) {
    errs() << "SVC-MHP: no main function found\n";
    return;
  }

  m_tfg->addThread(0, main_func);
  processFunction(main_func, 0, 0);
  wireSynchronizationEdges();
}

void StaticVectorClockMHP::processFunction(const Function *func, ThreadID tid,
                                          CallContextID ctx) {
  if (!func || func->isDeclaration())
    return;

  auto &visited = m_visited_functions_by_thread[tid][ctx];
  if (!visited.insert(func).second)
    return;

  // --- Pass 1: Create all nodes for this function ---
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      mapInstructionToThread(&inst, tid);
      SyncNodeType node_type = SyncNodeType::REGULAR_INST;

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        (void)cb;
        if (m_thread_api->isTDFork(&inst)) {
          node_type = SyncNodeType::THREAD_FORK;
        } else if (m_thread_api->isTDJoin(&inst)) {
          node_type = SyncNodeType::THREAD_JOIN;
        } else if (m_thread_api->isTDAcquire(&inst)) {
          node_type = SyncNodeType::LOCK_ACQUIRE;
        } else if (m_thread_api->isTDRelease(&inst)) {
          node_type = SyncNodeType::LOCK_RELEASE;
        } else if (m_thread_api->isTDExit(&inst)) {
          node_type = SyncNodeType::THREAD_EXIT;
        } else if (m_thread_api->isTDCondWait(&inst)) {
          node_type = SyncNodeType::COND_WAIT;
        } else if (m_thread_api->isTDCondSignal(&inst)) {
          node_type = SyncNodeType::COND_SIGNAL;
        } else if (m_thread_api->isTDCondBroadcast(&inst)) {
          node_type = SyncNodeType::COND_BROADCAST;
        } else if (m_thread_api->isTDBarWait(&inst)) {
          node_type = SyncNodeType::BARRIER_WAIT;
        }
      }
      m_tfg->createNode(&inst, node_type, tid, ctx);
    }
  }

  // --- Pass 2: Add edges and handle synchronization logic ---
  SyncNode *entry_node = nullptr;
  if (!func->empty() && !func->front().empty()) {
    entry_node = m_tfg->getNode(&func->front().front(), tid, ctx);
    if (entry_node && ctx == 0) {
      m_tfg->setThreadEntryNode(tid, entry_node);
    }
  }

  SyncNode *exit_node = nullptr;

  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      SyncNode *node = m_tfg->getNode(&inst, tid, ctx);
      if (!node) {
        continue;
      }

      exit_node = node;

      if (&inst != &bb.front()) {
        const Instruction *prev_inst = inst.getPrevNode();
        if (prev_inst) {
          SyncNode *prev_node = m_tfg->getNode(prev_inst, tid, ctx);
          if (prev_node) {
            m_tfg->addIntraThreadEdge(prev_node, node);
          }
        }
      }

      if (inst.isTerminator()) {
        for (const BasicBlock *succ : successors(inst.getParent())) {
          if (!succ->empty()) {
            SyncNode *succ_node = m_tfg->getNode(&succ->front(), tid, ctx);
            if (succ_node) {
              m_tfg->addIntraThreadEdge(node, succ_node);
            }
          }
        }
      }

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        if (m_thread_api->isTDFork(&inst)) {
          handleThreadFork(&inst, node, tid);
        } else if (m_thread_api->isTDJoin(&inst)) {
          handleThreadJoin(&inst, node, tid);
        } else if (m_thread_api->isTDAcquire(&inst)) {
          handleLockAcquire(&inst, node);
        } else if (m_thread_api->isTDRelease(&inst)) {
          handleLockRelease(&inst, node);
        } else if (m_thread_api->isTDCondWait(&inst)) {
          handleCondWait(&inst, node);
        } else if (m_thread_api->isTDCondSignal(&inst)) {
          handleCondSignal(&inst, node);
        } else if (m_thread_api->isTDCondBroadcast(&inst)) {
          handleCondSignal(&inst, node);
        } else if (m_thread_api->isTDBarWait(&inst)) {
          handleBarrier(&inst, node);
        } else {
          auto processCallee = [&](const Function *callee) {
            if (!callee || callee->isDeclaration())
              return;

            CallContextID callee_ctx = node->getNodeID();
            processFunction(callee, tid, callee_ctx);
            SyncNode *callee_entry =
                m_tfg->getNode(&callee->front().front(), tid, callee_ctx);
            if (callee_entry) {
              m_tfg->addCallEdge(node, callee_entry);
            }
            SyncNode *callee_exit =
                m_tfg->getFunctionExitNode(tid, callee, callee_ctx);
            if (!callee_exit) {
              return;
            }

            const Instruction *next_inst = inst.getNextNode();
            if (next_inst) {
              SyncNode *return_site = m_tfg->getNode(next_inst, tid, ctx);
              if (return_site) {
                m_tfg->addRetEdge(callee_exit, return_site);
                m_ret_to_call[return_site] = node;
              }
            } else if (inst.isTerminator()) {
              for (const BasicBlock *succ : successors(inst.getParent())) {
                if (succ->empty()) {
                  continue;
                }
                SyncNode *return_site = m_tfg->getNode(&succ->front(), tid, ctx);
                if (return_site) {
                  m_tfg->addRetEdge(callee_exit, return_site);
                  m_ret_to_call[return_site] = node;
                }
              }
            }
          };

          if (const Function *direct = m_thread_api->getCallee(cb)) {
            processCallee(direct);
          } else if (m_call_graph) {
            if (CallGraphNode *cgNode = (*m_call_graph)[cb->getFunction()]) {
              for (auto &callRecord : *cgNode) {
                if (!callRecord.first.hasValue() ||
                    dyn_cast_or_null<CallBase>(*callRecord.first) != cb) {
                  continue;
                }
                if (CallGraphNode *calleeNode = callRecord.second) {
                  processCallee(calleeNode->getFunction());
                }
              }
            }
          }
        }
      }
    }
  }

  if (exit_node) {
    if (!m_tfg->getThreadExitNode(tid)) {
      m_tfg->setThreadExitNode(tid, exit_node);
    }
    m_tfg->setFunctionExitNode(tid, func, exit_node, ctx);
  }
}

void StaticVectorClockMHP::mapInstructionToThread(const Instruction *inst,
                                                  ThreadID tid) {
  if (!inst) {
    return;
  }

  if (m_has_unresolved_fork &&
      m_thread_entry_candidates.find(inst->getFunction()) !=
          m_thread_entry_candidates.end()) {
    m_inst_to_thread[inst] = kUnknownThread;
    return;
  }

  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    m_inst_to_thread[inst] = tid;
    return;
  }

  if (it->second == kUnknownThread || it->second == tid) {
    return;
  }

  it->second = kUnknownThread;
}

bool StaticVectorClockMHP::isInstructionThreadAmbiguous(
    const Instruction *inst) const {
  if (!inst) {
    return true;
  }
  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    return true;
  }
  return it->second == kUnknownThread;
}

bool StaticVectorClockMHP::isMustIntraThreadEdge(const SyncNode *from,
                                                 const SyncNode *to) const {
  if (!from || !to) {
    return false;
  }
  if (from->getThreadID() != to->getThreadID()) {
    return false;
  }

  const Instruction *from_inst = from->getInstruction();
  const Instruction *to_inst = to->getInstruction();
  if (!from_inst || !to_inst) {
    return false;
  }

  if (from_inst->getParent() == to_inst->getParent()) {
    return from_inst->getNextNode() == to_inst;
  }

  const Function *func = from_inst->getFunction();
  if (!func || func != to_inst->getFunction()) {
    return false;
  }

  const PostDominatorTree &PDT = getPostDomTree(func);
  return PDT.dominates(to_inst->getParent(), from_inst->getParent());
}

const PostDominatorTree &
StaticVectorClockMHP::getPostDomTree(const Function *func) const {
  auto it = m_post_dom_cache.find(func);
  if (it != m_post_dom_cache.end()) {
    return *(it->second);
  }
  auto PDT = std::make_unique<PostDominatorTree>();
  PDT->recalculate(*const_cast<Function *>(func));
  auto *pdtPtr = PDT.get();
  m_post_dom_cache[func] = std::move(PDT);
  return *pdtPtr;
}

void StaticVectorClockMHP::enableIndirectForkConservatism() {
  if (m_has_unresolved_fork) {
    return;
  }
  m_has_unresolved_fork = true;

  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    if (!func.hasAddressTaken()) {
      continue;
    }

    m_thread_entry_candidates.insert(&func);
    for (Instruction &inst : instructions(func)) {
      auto it = m_inst_to_thread.find(&inst);
      if (it == m_inst_to_thread.end()) {
        m_inst_to_thread[&inst] = kUnknownThread;
      } else if (it->second != kUnknownThread) {
        it->second = kUnknownThread;
      }
    }
  }
}

ThreadID StaticVectorClockMHP::allocateThreadID() { return m_next_thread_id++; }

void StaticVectorClockMHP::handleThreadFork(const Instruction *fork_inst,
                                            SyncNode *node,
                                            ThreadID parent_tid) {
  ThreadID new_tid = allocateThreadID();

  node->setForkedThread(new_tid);

  m_thread_fork_sites[new_tid] = fork_inst;
  m_thread_parents[new_tid] = parent_tid;
  m_thread_children[parent_tid].push_back(new_tid);

  const Value *pthread_ptr = m_thread_api->getForkedThread(fork_inst);
  if (pthread_ptr) {
    m_pthread_value_to_thread[pthread_ptr] = new_tid;
    m_thread_to_pthread_value[new_tid] = pthread_ptr;
  }

  const Value *forked_fun_val = m_thread_api->getForkedFun(fork_inst);
  if (const Function *forked_fun = dyn_cast_or_null<Function>(forked_fun_val)) {
    m_tfg->addThread(new_tid, forked_fun);
    processFunction(forked_fun, new_tid, 0);
    if (SyncNode *child_entry = m_tfg->getThreadEntryNode(new_tid)) {
      m_tfg->addInterThreadEdge(node, child_entry);
    }
  } else {
    enableIndirectForkConservatism();
  }
}

void StaticVectorClockMHP::handleThreadJoin(const Instruction *join_inst,
                                            SyncNode *node,
                                            ThreadID parent_tid) {
  const Value *joined_thread_val = m_thread_api->getJoinedThread(join_inst);
  ThreadID joined_tid = 0;
  bool found = false;

  if (joined_thread_val) {
    auto it = m_pthread_value_to_thread.find(joined_thread_val);
    if (it != m_pthread_value_to_thread.end()) {
      joined_tid = it->second;
      found = true;
    } else if (const LoadInst *load = dyn_cast<LoadInst>(joined_thread_val)) {
      const Value *loaded_from = load->getPointerOperand();
      auto it2 = m_pthread_value_to_thread.find(loaded_from);
      if (it2 != m_pthread_value_to_thread.end()) {
        joined_tid = it2->second;
        found = true;
        m_pthread_value_to_thread[joined_thread_val] = joined_tid;
      }
    }
  }

  auto add_join_edge = [&](ThreadID tid) {
    if (SyncNode *child_exit = m_tfg->getThreadExitNode(tid)) {
      m_tfg->addInterThreadEdge(child_exit, node, EdgeKind::Join);
      node->setJoinedThread(tid);
      m_join_to_thread[join_inst] = tid;
    }
  };

  if (found && joined_tid != 0) {
    add_join_edge(joined_tid);
  } else {
    // Conservative per paper: when join target is unresolved, add HB from
    // every known thread exit to this join site so we don't weaken ordering.
    for (const auto &entry : m_tfg->getAllThreads()) {
      ThreadID other_tid = entry;
      if (other_tid == parent_tid || other_tid == 0)
        continue;
      add_join_edge(other_tid);
    }
  }
}

void StaticVectorClockMHP::handleLockAcquire(const Instruction *lock_inst,
                                             SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(lock_inst);
  node->setLockValue(lock);
}

void StaticVectorClockMHP::handleLockRelease(const Instruction *unlock_inst,
                                             SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(unlock_inst);
  node->setLockValue(lock);
}

void StaticVectorClockMHP::handleCondWait(const Instruction *wait_inst,
                                          SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(wait_inst);
  const Value *mutex = m_thread_api->getCondMutex(wait_inst);
  node->setCondValue(cond);
  node->setLockValue(mutex);
  m_condvar_waits[cond].push_back(node);
}

void StaticVectorClockMHP::handleCondSignal(const Instruction *signal_inst,
                                            SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(signal_inst);
  node->setCondValue(cond);
  m_condvar_signals[cond].push_back(node);
}

void StaticVectorClockMHP::handleBarrier(const Instruction *barrier_inst,
                                         SyncNode *node) {
  const Value *barrier = m_thread_api->getBarrierVal(barrier_inst);
  if (!barrier) {
    barrier = barrier_inst;
  }
  node->setLockValue(barrier);
  m_barrier_waits[barrier].push_back(node);
}

void StaticVectorClockMHP::wireSynchronizationEdges() {
  if (!m_tfg)
    return;
  // Do not synthesize definite condition-variable HB edges here. Without
  // waiter queue / phase analysis, connecting a signal to all waits can hide
  // real races by ordering waits that are resumed by a different signal or
  // that start waiting later.
  // B12 fix: barrier semantics require that every arrival HB every other
  // arrival, but adding edges in BOTH directions between the same pair
  // creates a cycle in the HB graph (n_i HB n_j AND n_j HB n_i).
  // Fix: only add the edge i→j for i < j (one direction per pair).
  // The SVC transfer function treats Barrier edges with SVMax, so a single
  // directed edge per pair is sufficient to enforce the all-before-all
  // barrier semantics without introducing cycles.
  for (const auto &kv : m_barrier_waits) {
    const std::vector<SyncNode *> &waits = kv.second;
    for (size_t i = 0; i < waits.size(); ++i) {
      SyncNode *ni = waits[i];
      if (!ni)
        continue;
      for (size_t j = i + 1; j < waits.size(); ++j) {
        SyncNode *nj = waits[j];
        if (nj && nj->getThreadID() != ni->getThreadID()) {
          m_tfg->addInterThreadEdge(ni, nj, EdgeKind::Barrier);
        }
      }
    }
  }
}
