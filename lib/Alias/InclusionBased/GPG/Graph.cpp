#include "Alias/InclusionBased/GPG/Graph.h"

#include "Alias/InclusionBased/GPG/InternedReaching.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <iterator>
#include <stack>
#include <tuple>
#include <utility>

namespace lotus::gpg {

namespace {

const GPBIdSet EMPTY_GPB_SET;

template <typename T> bool replaceSet(std::set<T> &target, std::set<T> value) {
  if (target == value)
    return false;
  target = std::move(value);
  return true;
}

GPUSet setUnion(const GPUSet &lhs, const GPUSet &rhs) {
  GPUSet result = lhs;
  result.insert(rhs.begin(), rhs.end());
  return result;
}

GPUSet setDifference(const GPUSet &lhs, const GPUSet &rhs) {
  GPUSet result;
  std::set_difference(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                      std::inserter(result, result.end()));
  return result;
}

GPUSet setIntersection(const GPUSet &lhs, const GPUSet &rhs) {
  GPUSet result;
  std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                        std::inserter(result, result.end()));
  return result;
}

AccessSet sources(const GPUSet &gpus) {
  AccessSet result;
  for (const GPU &gpu : gpus)
    result.insert(gpu.source);
  return result;
}

bool dataDependent(const GPUSet &lhs, const GPUSet &rhs,
                   const TypeCompatibility &compatible) {
  for (const GPU &left : lhs) {
    for (const GPU &right : rhs) {
      const bool definite =
          definiteDependence(left, right) != Dependence::None ||
          definiteDependence(right, left) != Dependence::None;
      if (!definite && (potentialDependence(left, right, compatible) ||
                        potentialDependence(right, left, compatible)))
        return true;
    }
  }
  return false;
}

} // namespace

bool GPB::isStructural() const {
  return kind == GPBKind::Start || kind == GPBKind::End ||
         kind == GPBKind::DirectCall || kind == GPBKind::IndirectCall ||
         kind == GPBKind::BottomCall;
}

void GPG::setBoundaryDefinitions(GPUSet definitions) {
  boundary_definitions_ = std::move(definitions);
}

bool GPG::hasBlock(GPBId id) const { return blocks_.count(id) != 0; }

GPB *GPG::getBlock(GPBId id) {
  auto it = blocks_.find(id);
  return it == blocks_.end() ? nullptr : &it->second;
}

const GPB *GPG::getBlock(GPBId id) const {
  auto it = blocks_.find(id);
  return it == blocks_.end() ? nullptr : &it->second;
}

void GPG::addBlock(GPB block) {
  predecessors_.try_emplace(block.id);
  successors_.try_emplace(block.id);
  blocks_[block.id] = std::move(block);
}

void GPG::addEdge(GPBId from, GPBId to) {
  if (!hasBlock(from) || !hasBlock(to))
    return;
  successors_[from].insert(to);
  predecessors_[to].insert(from);
}

void GPG::removeEdge(GPBId from, GPBId to) {
  successors_[from].erase(to);
  predecessors_[to].erase(from);
}

const GPBIdSet &GPG::predecessors(GPBId id) const {
  auto it = predecessors_.find(id);
  return it == predecessors_.end() ? EMPTY_GPB_SET : it->second;
}

const GPBIdSet &GPG::successors(GPBId id) const {
  auto it = successors_.find(id);
  return it == successors_.end() ? EMPTY_GPB_SET : it->second;
}

GPU GPG::makeBoundaryDefinition(const Access &source) {
  GPU boundary;
  boundary.source = source;
  boundary.target = source;
  boundary.target.upward_exposed = true;
  boundary.statement = 0;
  boundary.kind = GPUKind::Boundary;
  boundary.source_may_alias_multiple = source.upward_exposed;
  return boundary;
}

GPUSet GPG::reduceBlock(const GPB &block, const GPUProducerIndex &available,
                        const GPUProducerIndex &unblocked, unsigned k_limit,
                        GPUSet &queued) const {
  GPUSet generated;
  for (const GPU &gpu : block.gpus) {
    ReductionResult reduced = reduceGPU(gpu, available, unblocked, k_limit);
    generated.insert(reduced.reduced.begin(), reduced.reduced.end());
    queued.insert(reduced.queued.begin(), reduced.queued.end());
  }
  return generated;
}

GPUSet GPG::computeKills(const GPB &block, const GPUSet &incoming,
                         const GPUSet &generated) const {
  GPUSet killed;
  std::map<StatementId, GPUSet> by_statement;
  for (const GPU &gpu : generated)
    by_statement[gpu.statement].insert(gpu);

  for (const auto &[statement, statement_gpus] : by_statement) {
    (void)statement;
    const AccessSet defined_sources = sources(statement_gpus);
    if (defined_sources.size() != 1)
      continue;

    const Access &defined = *defined_sources.begin();
    const GPU &representative = *statement_gpus.begin();
    if (defined.upward_exposed || representative.source_may_alias_multiple ||
        block.may_definitions.count(defined) != 0)
      continue;

    for (const GPU &candidate : incoming) {
      if (candidate.source.sameSource(defined))
        killed.insert(candidate);
    }
  }
  return killed;
}

GPUSet GPG::computeBlocked(const GPUSet &incoming, const GPUSet &generated,
                           const TypeCompatibility &compatible) const {
  if (generated.empty())
    return {};

  GPUSet indirect_generated;
  for (const GPU &gpu : generated) {
    if (gpu.isIndirect())
      indirect_generated.insert(gpu);
  }

  GPUSet blocked;
  if (!indirect_generated.empty()) {
    for (const GPU &candidate : incoming) {
      if (candidate.flow_insensitive)
        continue;
      if (dataDependent(indirect_generated, {candidate}, compatible))
        blocked.insert(candidate);
    }
    return blocked;
  }

  for (const GPU &candidate : incoming) {
    if (!candidate.flow_insensitive && candidate.isIndirect() &&
        dataDependent(generated, {candidate}, compatible))
      blocked.insert(candidate);
  }
  return blocked;
}

ReachingState GPG::computeReaching(bool use_blocking,
                                   const ReachingState *without_blocking,
                                   const TypeCompatibility &compatible,
                                   unsigned k_limit) const {
  ReachingState state;
  std::deque<GPBId> worklist;
  std::set<GPBId> queued_blocks;
  for (GPBId id : reversePostOrder()) {
    worklist.push_back(id);
    queued_blocks.insert(id);
  }

  while (!worklist.empty()) {
    const GPBId id = worklist.front();
    worklist.pop_front();
    queued_blocks.erase(id);
    const GPB &block = blocks_.at(id);

    GPUSet incoming;
    if (id == entry_) {
      incoming = boundary_definitions_;
    } else {
      for (GPBId predecessor : predecessors(id)) {
        const GPUSet &out = state.out[predecessor];
        incoming.insert(out.begin(), out.end());
      }
    }

    GPUSet local_queued;
    const GPUSet &full_incoming = use_blocking && without_blocking
                                      ? without_blocking->in.at(id)
                                      : incoming;
    GPUProducerIndex available;
    available.add(incoming);
    available.add(support_gpus_);
    GPUProducerIndex full_available;
    const GPUProducerIndex *full_index = &available;
    if (&full_incoming != &incoming) {
      full_available.add(full_incoming);
      full_available.add(support_gpus_);
      full_index = &full_available;
    }
    GPUSet generated;
    GPUSet killed;
    if (block.kind == GPBKind::BottomCall) {
      // Delta-bottom is the constant empty transfer function used for the
      // first refinement of a recursive SCC (Section 7.2).
      killed = incoming;
    } else {
      generated =
          reduceBlock(block, available, *full_index, k_limit, local_queued);
      killed = computeKills(block, incoming, generated);
    }
    GPUSet blocked;
    if (use_blocking)
      blocked = computeBlocked(incoming, generated, compatible);
    killed.insert(blocked.begin(), blocked.end());

    GPUSet outgoing = setUnion(setDifference(incoming, killed), generated);
    if (use_blocking) {
      for (const GPU &gpu : blocked)
        outgoing.insert(makeBoundaryDefinition(gpu.source));
    }

    bool changed = false;
    changed |= replaceSet(state.in[id], std::move(incoming));
    changed |= replaceSet(state.generated[id], std::move(generated));
    changed |= replaceSet(state.killed[id], std::move(killed));
    changed |= replaceSet(state.blocked[id], std::move(blocked));
    changed |= replaceSet(state.out[id], std::move(outgoing));
    state.queued.insert(local_queued.begin(), local_queued.end());

    if (!changed)
      continue;
    for (GPBId successor : successors(id)) {
      if (queued_blocks.insert(successor).second)
        worklist.push_back(successor);
    }
  }

  return state;
}

ReachingPair GPG::analyzeReaching(const TypeCompatibility &compatible,
                                  unsigned k_limit) const {
  return analyzeReachingInterned(*this, compatible, k_limit);
}

bool GPG::strengthReduce(const TypeCompatibility &compatible, unsigned k_limit,
                         bool use_blocking) {
  ReachingPair reaching = analyzeReaching(compatible, k_limit);
  return applyStrengthReduction(reaching, use_blocking);
}

bool GPG::applyStrengthReduction(const ReachingPair &reaching,
                                 bool use_blocking) {
  const ReachingState &selected =
      use_blocking ? reaching.with_blocking : reaching.without_blocking;
  bool changed = false;

  for (auto &[id, block] : blocks_) {
    GPUSet replacement = selected.generated.at(id);
    if (block.gpus != replacement) {
      block.gpus = std::move(replacement);
      changed = true;
    }

    std::map<StatementId, AccessSet> statement_sources;
    for (const GPU &gpu : block.gpus)
      statement_sources[gpu.statement].insert(gpu.source);
    for (const auto &[statement, defined] : statement_sources) {
      (void)statement;
      if (defined.size() > 1)
        block.may_definitions.insert(defined.begin(), defined.end());
    }
    for (const GPU &gpu : block.gpus) {
      if (gpu.source.upward_exposed || gpu.source_may_alias_multiple)
        block.may_definitions.insert(gpu.source);
    }
  }
  return changed;
}

bool GPG::eliminateDeadGPUs(const TypeCompatibility &compatible,
                            unsigned k_limit, bool use_blocking,
                            const ReachingPair *precomputed) {
  if (!hasBlock(exit_))
    return false;
  ReachingPair computed;
  if (!precomputed) {
    computed = analyzeReaching(compatible, k_limit);
    precomputed = &computed;
  }
  const ReachingPair &reaching = *precomputed;
  GPUSet live = reaching.without_blocking.out.at(exit_);
  if (use_blocking)
    live = setUnion(live, reaching.with_blocking.out.at(exit_));
  live.insert(reaching.without_blocking.queued.begin(),
              reaching.without_blocking.queued.end());
  if (use_blocking) {
    live.insert(reaching.with_blocking.queued.begin(),
                reaching.with_blocking.queued.end());
  }

  bool changed = false;
  for (auto &[id, block] : blocks_) {
    (void)id;
    if (block.isStructural())
      continue;
    GPUSet retained = setIntersection(block.gpus, live);
    if (retained != block.gpus) {
      block.gpus = std::move(retained);
      changed = true;
    }
  }
  return changed;
}

bool GPG::eliminateEmptyGPBs() {
  std::vector<GPBId> removable;
  for (const auto &[id, block] : blocks_) {
    if (id != entry_ && id != exit_ && block.empty() && !block.isStructural())
      removable.push_back(id);
  }

  for (GPBId id : removable) {
    GPBIdSet pred = predecessors(id);
    GPBIdSet succ = successors(id);
    for (GPBId from : pred)
      removeEdge(from, id);
    for (GPBId to : succ)
      removeEdge(id, to);
    for (GPBId from : pred) {
      for (GPBId to : succ) {
        if (from != to)
          addEdge(from, to);
      }
    }
    predecessors_.erase(id);
    successors_.erase(id);
    blocks_.erase(id);
  }
  return !removable.empty();
}

void GPG::eraseBlock(GPBId id) {
  GPBIdSet pred = predecessors(id);
  GPBIdSet succ = successors(id);
  for (GPBId from : pred)
    removeEdge(from, id);
  for (GPBId to : succ)
    removeEdge(id, to);
  predecessors_.erase(id);
  successors_.erase(id);
  blocks_.erase(id);
}

bool GPG::expandCall(GPBId call_block_id,
                     const std::vector<CallExpansion> &alternatives) {
  GPB *call_block = getBlock(call_block_id);
  if (!call_block || alternatives.empty())
    return false;

  GPBIdSet call_predecessors = predecessors(call_block_id);
  GPBIdSet call_successors = successors(call_block_id);
  GPBId next_id = blocks_.empty() ? 1 : blocks_.rbegin()->first + 1;
  const bool alternatives_are_may = alternatives.size() > 1;

  auto strip_upward = [](Access access) {
    access.upward_exposed = false;
    return access;
  };
  auto copy_gpu = [&](GPU gpu) {
    gpu.source = strip_upward(gpu.source);
    gpu.target = strip_upward(gpu.target);
    return gpu;
  };

  for (const CallExpansion &alternative : alternatives) {
    if (!alternative.callee || !alternative.callee->validate())
      continue;

    GPB parameter;
    parameter.id = next_id++;
    parameter.kind = GPBKind::Parameter;
    parameter.callsite = call_block->callsite;
    for (const GPU &gpu : alternative.parameter_gpus)
      parameter.gpus.insert(copy_gpu(gpu));
    for (const GPU &gpu : alternative.callee->supportGPUs()) {
      GPU support = copy_gpu(gpu);
      support.flow_insensitive = false;
      parameter.gpus.insert(std::move(support));
    }
    parameter.original_gpus = parameter.gpus;
    if (alternatives_are_may)
      parameter.may_definitions = sources(parameter.gpus);
    const GPBId parameter_id = parameter.id;
    addBlock(std::move(parameter));

    std::map<GPBId, GPBId> cloned_ids;
    for (const auto &[callee_id, callee_block] : alternative.callee->blocks()) {
      GPB clone = callee_block;
      clone.id = next_id++;
      if (clone.kind == GPBKind::Start || clone.kind == GPBKind::End)
        clone.kind = GPBKind::Normal;

      GPUSet copied_gpus;
      for (const GPU &gpu : clone.gpus)
        copied_gpus.insert(copy_gpu(gpu));
      clone.gpus = std::move(copied_gpus);

      GPUSet copied_original;
      for (const GPU &gpu : clone.original_gpus)
        copied_original.insert(copy_gpu(gpu));
      clone.original_gpus = std::move(copied_original);

      AccessSet copied_may;
      for (const Access &source : clone.may_definitions)
        copied_may.insert(strip_upward(source));
      if (alternatives_are_may) {
        AccessSet clone_sources = sources(clone.gpus);
        copied_may.insert(clone_sources.begin(), clone_sources.end());
      }
      clone.may_definitions = std::move(copied_may);
      cloned_ids[callee_id] = clone.id;
      addBlock(std::move(clone));
    }

    for (const auto &[from, callee_block] : alternative.callee->blocks()) {
      (void)callee_block;
      for (GPBId to : alternative.callee->successors(from))
        addEdge(cloned_ids.at(from), cloned_ids.at(to));
    }

    GPB returns;
    returns.id = next_id++;
    returns.kind = GPBKind::Return;
    returns.callsite = call_block->callsite;
    for (const GPU &gpu : alternative.return_gpus)
      returns.gpus.insert(copy_gpu(gpu));
    returns.original_gpus = returns.gpus;
    if (alternatives_are_may)
      returns.may_definitions = sources(returns.gpus);
    const GPBId return_id = returns.id;
    addBlock(std::move(returns));

    for (GPBId predecessor : call_predecessors)
      addEdge(predecessor, parameter_id);
    addEdge(parameter_id, cloned_ids.at(alternative.callee->entry()));
    addEdge(cloned_ids.at(alternative.callee->exit()), return_id);
    for (GPBId successor : call_successors)
      addEdge(return_id, successor);
  }

  eraseBlock(call_block_id);
  return true;
}

bool GPG::replaceCallWithBottom(GPBId call_block_id) {
  GPB *block = getBlock(call_block_id);
  if (!block)
    return false;
  block->kind = GPBKind::BottomCall;
  block->gpus.clear();
  block->original_gpus.clear();
  block->may_definitions.clear();
  return true;
}

std::vector<GPBId> GPG::callBlocks() const {
  std::vector<GPBId> result;
  for (const auto &[id, block] : blocks_) {
    if (block.kind == GPBKind::DirectCall ||
        block.kind == GPBKind::IndirectCall)
      result.push_back(id);
  }
  return result;
}

bool GPG::preventsCoalescing(const GPU &later, const GPU &earlier,
                             const TypeCompatibility &compatible) const {
  Dependence definite = definiteDependence(later, earlier);
  if (hasDependence(definite, Dependence::ReadAfterWrite) ||
      hasDependence(definite, Dependence::WriteAfterWrite))
    return true;
  if (definite != Dependence::None)
    return false;
  return potentialDependence(later, earlier, compatible);
}

bool GPG::coalesce(const TypeCompatibility &compatible, unsigned k_limit) {
  (void)k_limit;
  if (blocks_.size() < 2)
    return false;

  ReachingPair reaching = analyzeReaching(compatible, k_limit);
  std::map<GPBId, GPBIdSet> parts;
  std::map<GPBId, GPBId> owner;
  for (const auto &[id, block] : blocks_) {
    (void)block;
    parts[id] = {id};
    owner[id] = id;
  }

  auto isCoherent = [&](const std::map<GPBId, GPBIdSet> &candidate_parts) {
    std::map<GPBId, GPBId> candidate_owner;
    for (const auto &[part_id, nodes] : candidate_parts) {
      for (GPBId node : nodes)
        candidate_owner[node] = part_id;
    }

    for (const auto &[part_id, nodes] : candidate_parts) {
      (void)part_id;
      std::set<GPBIdSet> external_predecessors;
      std::set<GPBIdSet> external_successors;
      for (GPBId node : nodes) {
        GPBIdSet pred;
        for (GPBId value : predecessors(node)) {
          if (candidate_owner[value] != candidate_owner[node])
            pred.insert(value);
        }
        if (!pred.empty())
          external_predecessors.insert(std::move(pred));

        GPBIdSet succ;
        for (GPBId value : successors(node)) {
          if (candidate_owner[value] != candidate_owner[node])
            succ.insert(value);
        }
        if (!succ.empty())
          external_successors.insert(std::move(succ));
      }
      if (external_predecessors.size() > 1 || external_successors.size() > 1)
        return false;
    }
    return true;
  };

  auto canMerge = [&](GPBId first, GPBId second) {
    GPBIdSet combined = parts[first];
    combined.insert(parts[second].begin(), parts[second].end());
    for (GPBId node : combined) {
      GPBKind kind = blocks_.at(node).kind;
      if (kind == GPBKind::DirectCall || kind == GPBKind::IndirectCall ||
          kind == GPBKind::BottomCall)
        return false;
    }
    for (GPBId earlier_id : combined) {
      for (GPBId later_id : combined) {
        if (earlier_id == later_id || !isReachable(earlier_id, later_id))
          continue;
        for (const GPU &earlier : blocks_.at(earlier_id).gpus) {
          for (const GPU &later : blocks_.at(later_id).gpus) {
            if (preventsCoalescing(later, earlier, compatible))
              return false;
          }
        }
      }
    }

    auto candidate = parts;
    candidate[first] = std::move(combined);
    candidate.erase(second);
    return isCoherent(candidate);
  };

  bool merged = true;
  bool changed = false;
  while (merged) {
    merged = false;
    for (GPBId from : reversePostOrder()) {
      std::vector<GPBId> next(successors(from).begin(), successors(from).end());
      for (GPBId to : next) {
        GPBId first = owner[from];
        GPBId second = owner[to];
        if (first == second || !canMerge(first, second))
          continue;
        parts[first].insert(parts[second].begin(), parts[second].end());
        for (GPBId node : parts[second])
          owner[node] = first;
        parts.erase(second);
        merged = true;
        changed = true;
        break;
      }
      if (merged)
        break;
    }
  }

  if (!changed)
    return false;

  std::map<GPBId, GPB> new_blocks;
  std::map<GPBId, GPBIdSet> new_predecessors;
  std::map<GPBId, GPBIdSet> new_successors;
  for (const auto &[part_id, nodes] : parts) {
    GPB combined;
    combined.id = part_id;
    combined.kind = nodes.count(entry_)  ? GPBKind::Start
                    : nodes.count(exit_) ? GPBKind::End
                    : nodes.size() == 1  ? blocks_.at(*nodes.begin()).kind
                                         : GPBKind::Normal;

    GPUSet in_gpus;
    GPUSet out_gpus;
    for (GPBId node : nodes) {
      const GPB &block = blocks_.at(node);
      combined.gpus.insert(block.gpus.begin(), block.gpus.end());
      combined.original_gpus.insert(block.original_gpus.begin(),
                                    block.original_gpus.end());
      combined.may_definitions.insert(block.may_definitions.begin(),
                                      block.may_definitions.end());
      if (!combined.origin_block)
        combined.origin_block = block.origin_block;
      if (!combined.callsite)
        combined.callsite = block.callsite;
      combined.callees.insert(block.callees.begin(), block.callees.end());

      bool is_entry = node == entry_;
      for (GPBId pred : predecessors(node))
        is_entry |= nodes.count(pred) == 0;
      if (is_entry) {
        const GPUSet &values = reaching.without_blocking.in[node];
        in_gpus.insert(values.begin(), values.end());
      }

      bool is_exit = node == exit_;
      for (GPBId succ : successors(node))
        is_exit |= nodes.count(succ) == 0;
      if (is_exit) {
        const GPUSet &values = reaching.without_blocking.out[node];
        out_gpus.insert(values.begin(), values.end());
      }
    }

    GPUSet preserved =
        setDifference(setIntersection(in_gpus, out_gpus), combined.gpus);
    AccessSet preserved_sources = sources(preserved);
    AccessSet defined_sources = sources(combined.gpus);
    for (const Access &source : defined_sources) {
      if (preserved_sources.count(source) != 0)
        combined.may_definitions.insert(source);
    }
    new_blocks[part_id] = std::move(combined);
  }

  for (const auto &[from, destinations] : successors_) {
    for (GPBId to : destinations) {
      GPBId new_from = owner[from];
      GPBId new_to = owner[to];
      if (new_from == new_to)
        continue;
      new_successors[new_from].insert(new_to);
      new_predecessors[new_to].insert(new_from);
    }
  }
  for (const auto &[id, block] : new_blocks) {
    (void)block;
    new_successors.try_emplace(id);
    new_predecessors.try_emplace(id);
  }

  entry_ = owner[entry_];
  exit_ = owner[exit_];
  blocks_ = std::move(new_blocks);
  predecessors_ = std::move(new_predecessors);
  successors_ = std::move(new_successors);
  return true;
}

bool GPG::validate() const {
  if (!hasBlock(entry_) || !hasBlock(exit_))
    return false;
  for (const auto &[from, destinations] : successors_) {
    if (!hasBlock(from))
      return false;
    for (GPBId to : destinations) {
      if (!hasBlock(to) || predecessors(to).count(from) == 0)
        return false;
    }
  }
  for (const auto &[to, sources] : predecessors_) {
    if (!hasBlock(to))
      return false;
    for (GPBId from : sources) {
      if (!hasBlock(from) || successors(from).count(to) == 0)
        return false;
    }
  }
  return true;
}

bool GPG::isBottom() const {
  const GPB *entry_block = getBlock(entry_);
  return entry_block && entry_block->kind == GPBKind::BottomCall;
}

bool GPG::isIdentity() const {
  for (const auto &[id, block] : blocks_) {
    (void)id;
    if (!block.gpus.empty() || block.kind == GPBKind::BottomCall)
      return false;
  }
  return true;
}

bool GPG::isReachable(GPBId from, GPBId to) const {
  if (from == to)
    return true;
  std::deque<GPBId> worklist = {from};
  GPBIdSet visited = {from};
  while (!worklist.empty()) {
    GPBId current = worklist.front();
    worklist.pop_front();
    for (GPBId successor : successors(current)) {
      if (successor == to)
        return true;
      if (visited.insert(successor).second)
        worklist.push_back(successor);
    }
  }
  return false;
}

std::vector<GPBId> GPG::reversePostOrder() const {
  std::vector<GPBId> postorder;
  GPBIdSet visited;
  std::function<void(GPBId)> visit = [&](GPBId id) {
    if (!visited.insert(id).second)
      return;
    for (GPBId successor : successors(id))
      visit(successor);
    postorder.push_back(id);
  };
  if (hasBlock(entry_))
    visit(entry_);
  for (const auto &[id, block] : blocks_) {
    (void)block;
    visit(id);
  }
  std::reverse(postorder.begin(), postorder.end());
  return postorder;
}

GPGStats GPG::stats() const {
  GPGStats result;
  result.blocks = blocks_.size();
  result.gpus = support_gpus_.size();
  for (const auto &[id, block] : blocks_) {
    (void)id;
    result.gpus += block.gpus.size();
  }
  for (const auto &[id, successors] : successors_) {
    (void)id;
    result.flow_edges += successors.size();
  }
  return result;
}

} // namespace lotus::gpg
