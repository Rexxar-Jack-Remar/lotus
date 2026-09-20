#include "Alias/InclusionBased/GPG/GPU.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>

namespace lotus::gpg {

bool Access::sameBase(const Access &other) const {
  return location == other.location && upward_exposed == other.upward_exposed;
}

bool Access::sameSource(const Access &other) const {
  return sameBase(other) && indirections.equivalentTo(other.indirections);
}

bool Access::operator==(const Access &other) const {
  return location == other.location && indirections == other.indirections &&
         upward_exposed == other.upward_exposed;
}

bool Access::operator<(const Access &other) const {
  return std::tie(location, indirections, upward_exposed) <
         std::tie(other.location, other.indirections, other.upward_exposed);
}

bool GPUQuery::operator==(const GPUQuery &other) const {
  return original == other.original && endpoint == other.endpoint;
}

bool GPUQuery::operator<(const GPUQuery &other) const {
  return std::tie(original, endpoint) <
         std::tie(other.original, other.endpoint);
}

bool GPU::isPointsToEdge() const {
  return source.indirections == IndirectionList::dereferences(1) &&
         target.indirections.empty();
}

std::string GPU::str() const {
  std::ostringstream out;
  out << source.location;
  if (source.upward_exposed)
    out << '\'';
  out << source.indirections.str() << " -> " << target.location;
  if (target.upward_exposed)
    out << '\'';
  out << target.indirections.str() << " @" << statement;
  return out.str();
}

bool GPU::operator==(const GPU &other) const {
  return source == other.source && target == other.target &&
         statement == other.statement && kind == other.kind &&
         source_may_alias_multiple == other.source_may_alias_multiple &&
         pointer_arithmetic == other.pointer_arithmetic &&
         flow_insensitive == other.flow_insensitive &&
         provenance == other.provenance;
}

bool GPU::operator<(const GPU &other) const {
  return std::tie(statement, provenance, kind, source, target,
                  source_may_alias_multiple, pointer_arithmetic,
                  flow_insensitive) <
         std::tie(other.statement, other.provenance, other.kind, other.source,
                  other.target, other.source_may_alias_multiple,
                  other.pointer_arithmetic, other.flow_insensitive);
}

static bool pivotMatches(const Access &consumer, const Access &producer) {
  return consumer.sameBase(producer);
}

static bool containsAnyField(const IndirectionList &list) {
  return std::any_of(list.elements().begin(), list.elements().end(),
                     [](const Indirection &step) {
                       return step.kind == IndirectionKind::AnyField;
                     });
}

CompositionResult composeGPU(const GPU &consumer, const GPU &producer,
                             CompositionKind kind, unsigned k_limit) {
  CompositionResult result;

  const Access *consumer_pivot = nullptr;
  if (kind == CompositionKind::TargetSource)
    consumer_pivot = &consumer.target;
  else
    consumer_pivot = &consumer.source;

  if (!pivotMatches(*consumer_pivot, producer.source))
    return result;

  const bool prefix =
      producer.source.indirections.isPrefixOf(consumer_pivot->indirections);
  const bool proper_prefix = producer.source.indirections.isProperPrefixOf(
      consumer_pivot->indirections);
  result.valid = kind == CompositionKind::TargetSource ? prefix : proper_prefix;
  if (!result.valid)
    return result;

  const unsigned effective_limit = consumer_pivot->k_limited ||
                                           producer.source.k_limited ||
                                           producer.target.k_limited
                                       ? k_limit
                                       : 0;
  auto remainders = consumer_pivot->indirections.remaindersAfter(
      producer.source.indirections, effective_limit);
  for (const IndirectionList &remainder : remainders) {
    GPU composed = consumer;
    IndirectionList replacement =
        producer.target.indirections.append(remainder, effective_limit);

    if (kind == CompositionKind::TargetSource) {
      composed.target = producer.target;
      composed.target.indirections = std::move(replacement);
      if (!composed.target.indirections.doesNotExceed(
              consumer.target.indirections))
        continue;
    } else {
      composed.source = producer.target;
      composed.source.indirections = std::move(replacement);
      composed.source_may_alias_multiple =
          consumer.source_may_alias_multiple ||
          producer.source_may_alias_multiple ||
          producer.target.upward_exposed || producer.target.k_limited ||
          containsAnyField(composed.source.indirections);
      if (!composed.source.indirections.doesNotExceed(
              consumer.source.indirections))
        continue;
    }
    result.gpus.insert(std::move(composed));
  }

  result.desirable = !result.gpus.empty();
  return result;
}

Dependence operator|(Dependence lhs, Dependence rhs) {
  return static_cast<Dependence>(static_cast<unsigned>(lhs) |
                                 static_cast<unsigned>(rhs));
}

bool hasDependence(Dependence value, Dependence kind) {
  return (static_cast<unsigned>(value) & static_cast<unsigned>(kind)) != 0;
}

Dependence definiteDependence(const GPU &consumer, const GPU &producer) {
  Dependence result = Dependence::None;

  if (consumer.source.sameSource(producer.source))
    result = result | Dependence::WriteAfterWrite;

  if (consumer.source.sameBase(producer.source) &&
      consumer.source.indirections.isProperPrefixOf(
          producer.source.indirections))
    result = result | Dependence::WriteAfterRead;
  if (consumer.source.sameBase(producer.target) &&
      consumer.source.indirections.isPrefixOf(producer.target.indirections))
    result = result | Dependence::WriteAfterRead;

  if (consumer.source.sameBase(producer.source) &&
      producer.source.indirections.isProperPrefixOf(
          consumer.source.indirections))
    result = result | Dependence::ReadAfterWrite;
  if (consumer.target.sameBase(producer.source) &&
      producer.source.indirections.isPrefixOf(consumer.target.indirections))
    result = result | Dependence::ReadAfterWrite;

  return result;
}

bool potentialDependence(const GPU &consumer, const GPU &producer,
                         const TypeCompatibility &compatible) {
  if (definiteDependence(consumer, producer) != Dependence::None)
    return true;

  if (consumer.source.sameBase(producer.source)) {
    const auto &left = consumer.source.indirections.elements();
    const auto &right = producer.source.indirections.elements();
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < count; ++index) {
      if (left[index].kind == IndirectionKind::Field &&
          right[index].kind == IndirectionKind::Field &&
          left[index].field != right[index].field)
        return false;
      if (!left[index].matches(right[index]))
        break;
    }
  }

  if (!consumer.isIndirect() && !producer.isIndirect())
    return false;

  if (!compatible)
    return true;

  const bool def_def = compatible(consumer.source.type, producer.source.type);
  const bool def_ref = compatible(consumer.source.type, producer.target.type);
  const bool ref_def = compatible(consumer.target.type, producer.source.type);
  return def_def || def_ref || ref_def;
}

namespace {

struct ReductionState {
  GPU gpu;
  std::set<const GPU *> used;
};

GPUSet composeWithProducer(const GPU &consumer, const GPU &producer,
                           unsigned k_limit, bool &valid, bool &desirable) {
  const CompositionResult ts =
      composeGPU(consumer, producer, CompositionKind::TargetSource, k_limit);
  const CompositionResult ss =
      composeGPU(consumer, producer, CompositionKind::SourceSource, k_limit);
  valid = ts.valid || ss.valid;
  desirable = ts.desirable || ss.desirable;

  if (ts.desirable && ss.desirable) {
    GPUSet both;
    for (const GPU &gpu : ts.gpus) {
      CompositionResult next =
          composeGPU(gpu, producer, CompositionKind::SourceSource, k_limit);
      both.insert(next.gpus.begin(), next.gpus.end());
    }
    for (const GPU &gpu : ss.gpus) {
      CompositionResult next =
          composeGPU(gpu, producer, CompositionKind::TargetSource, k_limit);
      both.insert(next.gpus.begin(), next.gpus.end());
    }
    if (!both.empty())
      return both;
  }

  GPUSet result = ts.gpus;
  result.insert(ss.gpus.begin(), ss.gpus.end());
  return result;
}

} // namespace

void GPUProducerIndex::add(const GPUSet &gpus) {
  for (const GPU &producer : gpus) {
    BaseKey key = {producer.source.location, producer.source.upward_exposed};
    producers_[key].push_back(&producer);
  }
}

std::vector<const GPU *>
GPUProducerIndex::candidates(const GPU &consumer) const {
  std::set<const GPU *> unique;
  auto add_pivot = [&](const Access &pivot) {
    auto found = producers_.find({pivot.location, pivot.upward_exposed});
    if (found != producers_.end())
      unique.insert(found->second.begin(), found->second.end());
  };
  add_pivot(consumer.source);
  add_pivot(consumer.target);
  return {unique.begin(), unique.end()};
}

bool GPUProducerIndex::contains(const GPU &gpu) const {
  auto found =
      producers_.find({gpu.source.location, gpu.source.upward_exposed});
  if (found == producers_.end())
    return false;
  return std::any_of(found->second.begin(), found->second.end(),
                     [&](const GPU *candidate) { return *candidate == gpu; });
}

ReductionResult reduceGPU(const GPU &consumer,
                          const GPUProducerIndex &available,
                          const GPUProducerIndex &unblocked, unsigned k_limit) {
  ReductionResult result;
  std::deque<ReductionState> worklist;
  worklist.push_back({consumer, {}});
  std::set<std::pair<GPU, std::set<const GPU *>>> visited;

  while (!worklist.empty()) {
    ReductionState state = std::move(worklist.front());
    worklist.pop_front();
    if (!visited.insert({state.gpu, state.used}).second)
      continue;

    bool progressed = false;
    for (const GPU *producer : available.candidates(state.gpu)) {
      if (state.used.count(producer) != 0)
        continue;
      bool valid = false;
      bool desirable = false;
      GPUSet composed =
          composeWithProducer(state.gpu, *producer, k_limit, valid, desirable);
      if (valid && !desirable)
        result.queued.insert(*producer);
      if (composed.empty())
        continue;
      progressed = true;
      std::set<const GPU *> next_used = state.used;
      next_used.insert(producer);
      for (const GPU &gpu : composed)
        worklist.push_back({gpu, next_used});
    }
    if (!progressed)
      result.reduced.insert(state.gpu);
  }

  if (&available != &unblocked) {
    for (const GPU *producer : unblocked.candidates(consumer)) {
      if (available.contains(*producer))
        continue;
      bool valid = false;
      bool desirable = false;
      (void)composeWithProducer(consumer, *producer, k_limit, valid, desirable);
      if (valid)
        result.queued.insert(*producer);
    }
  }
  return result;
}

ReductionResult reduceGPU(const GPU &consumer, const GPUSet &available,
                          const GPUSet &unblocked, unsigned k_limit) {
  GPUProducerIndex available_index;
  available_index.add(available);
  if (&available == &unblocked)
    return reduceGPU(consumer, available_index, available_index, k_limit);
  GPUProducerIndex unblocked_index;
  unblocked_index.add(unblocked);
  return reduceGPU(consumer, available_index, unblocked_index, k_limit);
}

} // namespace lotus::gpg
