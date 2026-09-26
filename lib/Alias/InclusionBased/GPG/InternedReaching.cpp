#include "Alias/InclusionBased/GPG/InternedReaching.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SparseBitVector.h>

namespace lotus::gpg {
namespace {

using GPUId = std::uint32_t;
using GPUIdSet = llvm::SparseBitVector<>;

bool isSubset(const GPUIdSet &subset, const GPUIdSet &superset) {
  for (GPUId id : subset) {
    if (!superset.test(id))
      return false;
  }
  return true;
}

std::size_t hashAccess(const Access &access) {
  std::size_t hash = static_cast<std::size_t>(llvm::hash_combine(
      access.location, access.upward_exposed,
      access.indirections.isSummarized()));
  for (const Indirection &step : access.indirections.elements()) {
    const std::int64_t field =
        step.kind == IndirectionKind::Field ? step.field : 0;
    hash = static_cast<std::size_t>(
        llvm::hash_combine(hash, static_cast<unsigned>(step.kind), field));
  }
  return hash;
}

struct GPUHash {
  std::size_t operator()(const GPU &gpu) const {
    return static_cast<std::size_t>(llvm::hash_combine(
        gpu.statement, gpu.provenance, static_cast<unsigned>(gpu.kind),
        hashAccess(gpu.source), hashAccess(gpu.target),
        gpu.source_may_alias_multiple, gpu.pointer_arithmetic,
        gpu.flow_insensitive));
  }
};

class GPUStore {
public:
  explicit GPUStore(std::size_t expected_gpus) {
    values_.reserve(expected_gpus + 1);
    ids_.reserve(expected_gpus * 2 + 1);
    values_.push_back({});
  }

  GPUId intern(const GPU &gpu) {
    auto found = ids_.find(gpu);
    if (found != ids_.end())
      return found->second;
    if (values_.size() > std::numeric_limits<GPUId>::max())
      throw std::overflow_error("GPG GPU ID space exhausted");

    const GPUId id = static_cast<GPUId>(values_.size());
    values_.push_back(gpu);
    ids_.emplace(values_.back(), id);

    return id;
  }

  const GPU &get(GPUId id) const { return values_.at(id); }

  GPUSet materialize(const GPUIdSet &ids) const {
    GPUSet result;
    for (GPUId id : ids)
      result.insert(get(id));
    return result;
  }

  static std::size_t sourceKey(const Access &access) {
    return static_cast<std::size_t>(access.location) * 2u +
           static_cast<std::size_t>(access.upward_exposed);
  }

private:
  std::vector<GPU> values_;
  std::unordered_map<GPU, GPUId, GPUHash> ids_;
};

class ProducerBuckets {
public:
  ProducerBuckets() = default;

  ProducerBuckets(const GPUStore &store, const GPUIdSet &ids) {
    build(store, ids);
  }

  void build(const GPUStore &store, const GPUIdSet &ids) {
    std::vector<std::size_t> counts;
    std::size_t total = 0;
    for (GPUId id : ids) {
      const std::size_t key = GPUStore::sourceKey(store.get(id).source);
      if (counts.size() <= key)
        counts.resize(key + 1);
      ++counts[key];
      ++total;
    }

    offsets_.resize(counts.size() + 1);
    for (std::size_t key = 0; key < counts.size(); ++key)
      offsets_[key + 1] = offsets_[key] + counts[key];
    values_.resize(total);
    std::vector<std::size_t> cursor = offsets_;
    for (GPUId id : ids) {
      const std::size_t key = GPUStore::sourceKey(store.get(id).source);
      values_[cursor[key]++] = id;
    }
  }

  template <typename Callback>
  void forEachMerged(const Access &pivot, const ProducerBuckets &other,
                     Callback &callback) const {
    const std::size_t key = GPUStore::sourceKey(pivot);
    std::size_t left = key + 1 < offsets_.size() ? offsets_[key] : 0;
    const std::size_t left_end =
        key + 1 < offsets_.size() ? offsets_[key + 1] : 0;
    std::size_t right =
        key + 1 < other.offsets_.size() ? other.offsets_[key] : 0;
    const std::size_t right_end =
        key + 1 < other.offsets_.size() ? other.offsets_[key + 1] : 0;

    while (left != left_end || right != right_end) {
      if (right == right_end ||
          (left != left_end && values_[left] < other.values_[right])) {
        callback(values_[left++]);
      } else if (left == left_end || other.values_[right] < values_[left]) {
        callback(other.values_[right++]);
      } else {
        callback(values_[left]);
        ++left;
        ++right;
      }
    }
  }

private:
  std::vector<std::size_t> offsets_;
  std::vector<GPUId> values_;
};

class ProducerIndex {
public:
  ProducerIndex(const GPUStore &store, const GPUIdSet &primary,
                const ProducerBuckets &support_buckets)
      : primary_buckets_(store, primary), support_buckets_(support_buckets) {}

  template <typename Callback>
  void forEachCandidate(const GPU &consumer, Callback callback) const {
    const std::size_t source_key = GPUStore::sourceKey(consumer.source);
    visitBucket(consumer.source, callback);
    if (GPUStore::sourceKey(consumer.target) != source_key)
      visitBucket(consumer.target, callback);
  }

private:
  template <typename Callback>
  void visitBucket(const Access &pivot, Callback &callback) const {
    primary_buckets_.forEachMerged(pivot, support_buckets_, callback);
  }

  ProducerBuckets primary_buckets_;
  const ProducerBuckets &support_buckets_;
};

GPUSet composeWithProducer(const GPU &consumer, const GPU &producer,
                           unsigned k_limit, bool &valid, bool &desirable,
                           bool &postponed) {
  CompositionResult ts = composeGPU(
      consumer, producer, CompositionKind::TargetSource, k_limit);
  CompositionResult ss = composeGPU(
      consumer, producer, CompositionKind::SourceSource, k_limit);
  valid = ts.valid || ss.valid;
  desirable = ts.desirable || ss.desirable;
  postponed = (ts.valid && !ts.desirable) || (ss.valid && !ss.desirable);

  if (ts.desirable && ss.desirable) {
    GPUSet both;
    for (const GPU &gpu : ts.gpus) {
      CompositionResult next = composeGPU(
          gpu, producer, CompositionKind::SourceSource, k_limit);
      both.insert(next.gpus.begin(), next.gpus.end());
    }
    for (const GPU &gpu : ss.gpus) {
      CompositionResult next = composeGPU(
          gpu, producer, CompositionKind::TargetSource, k_limit);
      both.insert(next.gpus.begin(), next.gpus.end());
    }
    if (!both.empty())
      return both;
  }

  GPUSet result = std::move(ts.gpus);
  result.insert(ss.gpus.begin(), ss.gpus.end());
  return result;
}

struct IdReductionResult {
  GPUIdSet reduced;
  GPUIdSet queued;
};

struct CachedComposition {
  GPUIdSet gpus;
  bool valid = false;
  bool desirable = false;
  bool postponed = false;
};

struct ReductionState {
  GPUId gpu = 0;
  GPUIdSet used;
};

struct IdState {
  explicit IdState(std::size_t block_count)
      : in(block_count), out(block_count), generated(block_count),
        killed(block_count), blocked(block_count) {}

  std::vector<GPUIdSet> in;
  std::vector<GPUIdSet> out;
  std::vector<GPUIdSet> generated;
  std::vector<GPUIdSet> killed;
  std::vector<GPUIdSet> blocked;
  GPUIdSet queued;
};

class InternedAnalysis {
public:
  InternedAnalysis(const GPG &graph, const TypeCompatibility &compatible,
                   unsigned k_limit)
      : graph_(graph), compatible_(compatible), k_limit_(k_limit),
        store_(initialGPUCount(graph)) {
    buildBlockLayout();
    for (const GPU &gpu : graph_.supportGPUs())
      support_.set(store_.intern(gpu));
    support_buckets_.build(store_, support_);
    for (const GPU &gpu : graph_.boundaryDefinitions())
      boundary_.set(store_.intern(gpu));
    for (std::size_t index = 0; index < blocks_.size(); ++index) {
      for (const GPU &gpu : blocks_[index]->gpus)
        block_gpus_[index].set(store_.intern(gpu));
    }
  }

  ReachingPair run() {
    IdState without = compute(false, nullptr);
    IdState with = compute(true, &without);
    return {materialize(without), materialize(with)};
  }

private:
  static std::size_t initialGPUCount(const GPG &graph) {
    std::size_t count = graph.supportGPUs().size() +
                        graph.boundaryDefinitions().size();
    for (const auto &[id, block] : graph.blocks()) {
      (void)id;
      count += block.gpus.size();
    }
    return count;
  }

  void buildBlockLayout() {
    std::unordered_set<GPBId> reachable;
    std::deque<GPBId> pending;
    if (graph_.hasBlock(graph_.entry())) {
      reachable.insert(graph_.entry());
      pending.push_back(graph_.entry());
    }
    while (!pending.empty()) {
      const GPBId id = pending.front();
      pending.pop_front();
      for (GPBId successor : graph_.successors(id)) {
        if (reachable.insert(successor).second)
          pending.push_back(successor);
      }
    }

    for (GPBId id : graph_.reversePostOrder()) {
      if (reachable.count(id) != 0)
        block_ids_.push_back(id);
    }
    blocks_.reserve(block_ids_.size());
    block_indices_.reserve(block_ids_.size() * 2 + 1);
    for (std::size_t index = 0; index < block_ids_.size(); ++index) {
      block_indices_.emplace(block_ids_[index], index);
      blocks_.push_back(graph_.getBlock(block_ids_[index]));
    }

    predecessors_.resize(block_ids_.size());
    successors_.resize(block_ids_.size());
    block_gpus_.resize(block_ids_.size());
    for (std::size_t index = 0; index < block_ids_.size(); ++index) {
      for (GPBId predecessor : graph_.predecessors(block_ids_[index])) {
        auto found = block_indices_.find(predecessor);
        if (found != block_indices_.end())
          predecessors_[index].push_back(found->second);
      }
      for (GPBId successor : graph_.successors(block_ids_[index])) {
        auto found = block_indices_.find(successor);
        if (found != block_indices_.end())
          successors_[index].push_back(found->second);
      }
    }
    auto entry = block_indices_.find(graph_.entry());
    entry_index_ = entry == block_indices_.end() ? block_ids_.size()
                                                  : entry->second;
  }

  const CachedComposition &composition(GPUId consumer_id,
                                       GPUId producer_id) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(consumer_id) << 32u) | producer_id;
    auto found = composition_cache_.find(key);
    if (found != composition_cache_.end())
      return found->second;

    CachedComposition cached;
    GPUSet composed = composeWithProducer(
        store_.get(consumer_id), store_.get(producer_id), k_limit_,
        cached.valid, cached.desirable, cached.postponed);
    for (const GPU &gpu : composed)
      cached.gpus.set(store_.intern(gpu));
    return composition_cache_.emplace(key, std::move(cached)).first->second;
  }

  IdReductionResult reduceGPU(GPUId consumer_id,
                              const ProducerIndex &available,
                              const ProducerIndex *blocked) {
    IdReductionResult result;
    std::deque<ReductionState> worklist = {{consumer_id, {}}};
    std::unordered_map<GPUId, std::vector<GPUIdSet>> visited;

    auto admit = [&](const ReductionState &state) {
      std::vector<GPUIdSet> &antichain = visited[state.gpu];
      for (const GPUIdSet &used : antichain) {
        if (isSubset(used, state.used))
          return false;
      }
      antichain.erase(
          std::remove_if(antichain.begin(), antichain.end(),
                         [&](const GPUIdSet &used) {
                           return isSubset(state.used, used);
                         }),
          antichain.end());
      antichain.push_back(state.used);
      return true;
    };

    while (!worklist.empty()) {
      ReductionState state = std::move(worklist.front());
      worklist.pop_front();
      if (!admit(state))
        continue;

      bool progressed = false;
      available.forEachCandidate(store_.get(state.gpu),
                                 [&](GPUId producer_id) {
        if (state.used.test(producer_id))
          return;
        const CachedComposition &composed =
            composition(state.gpu, producer_id);
        if (composed.postponed)
          result.queued.set(producer_id);
        if (composed.gpus.empty())
          return;
        progressed = true;
        GPUIdSet used = state.used;
        used.set(producer_id);
        for (GPUId gpu_id : composed.gpus)
          worklist.push_back({gpu_id, used});
      });

      if (blocked) {
        blocked->forEachCandidate(store_.get(state.gpu),
                                  [&](GPUId producer_id) {
          if (!state.used.test(producer_id) &&
              composition(state.gpu, producer_id).valid)
            result.queued.set(producer_id);
        });
      }
      if (!progressed)
        result.reduced.set(state.gpu);
    }
    return result;
  }

  GPUIdSet reduceBlock(std::size_t index, const ProducerIndex &available,
                       const ProducerIndex *blocked, GPUIdSet &queued) {
    GPUIdSet generated;
    for (GPUId consumer : block_gpus_[index]) {
      IdReductionResult reduced = reduceGPU(
          consumer, available, blocked);
      generated |= reduced.reduced;
      queued |= reduced.queued;
    }
    return generated;
  }

  GPUIdSet kills(const GPB &block, const GPUIdSet &incoming,
                 const GPUIdSet &generated) const {
    struct StatementGroup {
      GPUId representative = 0;
      GPUId first_source = 0;
      bool multiple_sources = false;
    };

    std::map<StatementId, StatementGroup> by_statement;
    for (GPUId id : generated) {
      const GPU &gpu = store_.get(id);
      StatementGroup &group = by_statement[gpu.statement];
      if (group.representative == 0 ||
          gpu < store_.get(group.representative))
        group.representative = id;
      if (group.first_source == 0) {
        group.first_source = id;
      } else if (!(gpu.source == store_.get(group.first_source).source)) {
        group.multiple_sources = true;
      }
    }

    GPUIdSet result;
    for (const auto &[statement, group] : by_statement) {
      (void)statement;
      if (group.multiple_sources)
        continue;
      const Access &source = store_.get(group.first_source).source;
      const GPU &representative = store_.get(group.representative);
      if (source.upward_exposed ||
          representative.source_may_alias_multiple ||
          block.may_definitions.count(source) != 0)
        continue;
      for (GPUId candidate : incoming) {
        if (store_.get(candidate).source.sameSource(source))
          result.set(candidate);
      }
    }
    return result;
  }

  bool unresolvedDependence(GPUId left, GPUId right) {
    const GPUId low = std::min(left, right);
    const GPUId high = std::max(left, right);
    const std::uint64_t key =
        (static_cast<std::uint64_t>(low) << 32u) | high;
    auto found = dependence_cache_.find(key);
    if (found != dependence_cache_.end())
      return found->second;

    const GPU &left_gpu = store_.get(left);
    const GPU &right_gpu = store_.get(right);
    bool unresolved = false;
    if (definiteDependence(left_gpu, right_gpu) == Dependence::None &&
        definiteDependence(right_gpu, left_gpu) == Dependence::None) {
      unresolved = potentialDependence(left_gpu, right_gpu, compatible_) ||
                   potentialDependence(right_gpu, left_gpu, compatible_);
    }
    dependence_cache_.emplace(key, unresolved);
    return unresolved;
  }

  GPUIdSet blocked(const GPUIdSet &incoming,
                   const GPUIdSet &generated) {
    if (generated.empty())
      return {};
    GPUIdSet indirect_generated;
    for (GPUId id : generated) {
      if (store_.get(id).isIndirect())
        indirect_generated.set(id);
    }

    GPUIdSet result;
    if (!indirect_generated.empty()) {
      for (GPUId candidate : incoming) {
        if (store_.get(candidate).flow_insensitive)
          continue;
        for (GPUId barrier : indirect_generated) {
          if (unresolvedDependence(barrier, candidate)) {
            result.set(candidate);
            break;
          }
        }
      }
      return result;
    }

    for (GPUId candidate : incoming) {
      const GPU &gpu = store_.get(candidate);
      if (gpu.flow_insensitive || !gpu.isIndirect())
        continue;
      for (GPUId barrier : generated) {
        if (unresolvedDependence(barrier, candidate)) {
          result.set(candidate);
          break;
        }
      }
    }
    return result;
  }

  IdState compute(bool use_blocking, const IdState *without) {
    IdState state(blocks_.size());
    std::deque<std::size_t> worklist;
    std::vector<bool> scheduled(blocks_.size(), true);
    for (std::size_t index = 0; index < blocks_.size(); ++index)
      worklist.push_back(index);

    while (!worklist.empty()) {
      const std::size_t index = worklist.front();
      worklist.pop_front();
      scheduled[index] = false;
      const GPB &block = *blocks_[index];

      GPUIdSet incoming;
      if (index == entry_index_) {
        incoming = boundary_;
      } else {
        for (std::size_t predecessor : predecessors_[index])
          incoming |= state.out[predecessor];
      }

      ProducerIndex available(store_, incoming, support_buckets_);
      GPUIdSet hidden;
      std::optional<ProducerIndex> hidden_index;
      if (use_blocking && without) {
        hidden.intersectWithComplement(without->in[index], incoming);
        hidden.intersectWithComplement(support_);
        hidden_index.emplace(store_, hidden, empty_buckets_);
      }

      GPUIdSet local_queued;
      GPUIdSet generated;
      GPUIdSet killed;
      if (block.kind == GPBKind::BottomCall) {
        killed = incoming;
      } else {
        generated = reduceBlock(index, available,
                                hidden_index ? &*hidden_index : nullptr,
                                local_queued);
        killed = kills(block, incoming, generated);
      }
      GPUIdSet blocked_values;
      if (use_blocking)
        blocked_values = blocked(incoming, generated);
      killed |= blocked_values;

      GPUIdSet outgoing = incoming - killed;
      outgoing |= generated;
      if (use_blocking) {
        for (GPUId blocked_id : blocked_values) {
          GPU boundary = GPG::makeBoundaryDefinition(
              store_.get(blocked_id).source);
          outgoing.set(store_.intern(boundary));
        }
      }

      const bool changed = state.in[index] != incoming ||
                           state.out[index] != outgoing ||
                           state.generated[index] != generated ||
                           state.killed[index] != killed ||
                           state.blocked[index] != blocked_values;
      state.in[index] = std::move(incoming);
      state.out[index] = std::move(outgoing);
      state.generated[index] = std::move(generated);
      state.killed[index] = std::move(killed);
      state.blocked[index] = std::move(blocked_values);
      state.queued |= local_queued;
      if (!changed)
        continue;
      for (std::size_t successor : successors_[index]) {
        if (!scheduled[successor]) {
          scheduled[successor] = true;
          worklist.push_back(successor);
        }
      }
    }
    return state;
  }

  ReachingState materialize(const IdState &state) const {
    ReachingState result;
    for (std::size_t index = 0; index < block_ids_.size(); ++index) {
      const GPBId id = block_ids_[index];
      result.in.emplace(id, store_.materialize(state.in[index]));
      result.out.emplace(id, store_.materialize(state.out[index]));
      result.generated.emplace(id, store_.materialize(state.generated[index]));
      result.killed.emplace(id, store_.materialize(state.killed[index]));
      result.blocked.emplace(id, store_.materialize(state.blocked[index]));
    }
    for (const auto &[id, block] : graph_.blocks()) {
      (void)block;
      result.in.try_emplace(id);
      result.out.try_emplace(id);
      result.generated.try_emplace(id);
      result.killed.try_emplace(id);
      result.blocked.try_emplace(id);
    }
    result.queued = store_.materialize(state.queued);
    return result;
  }

  const GPG &graph_;
  const TypeCompatibility &compatible_;
  unsigned k_limit_;
  GPUStore store_;
  GPUIdSet support_;
  ProducerBuckets support_buckets_;
  ProducerBuckets empty_buckets_;
  GPUIdSet boundary_;
  std::vector<GPBId> block_ids_;
  std::vector<const GPB *> blocks_;
  std::unordered_map<GPBId, std::size_t> block_indices_;
  std::vector<std::vector<std::size_t>> predecessors_;
  std::vector<std::vector<std::size_t>> successors_;
  std::vector<GPUIdSet> block_gpus_;
  std::size_t entry_index_ = 0;
  std::unordered_map<std::uint64_t, CachedComposition> composition_cache_;
  std::unordered_map<std::uint64_t, bool> dependence_cache_;
};

} // namespace

ReachingPair analyzeReachingInterned(const GPG &graph,
                                     const TypeCompatibility &compatible,
                                     unsigned k_limit) {
  return InternedAnalysis(graph, compatible, k_limit).run();
}

} // namespace lotus::gpg
