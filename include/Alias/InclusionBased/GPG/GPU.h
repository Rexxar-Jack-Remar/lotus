#pragma once

#include "Alias/InclusionBased/GPG/IndirectionList.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace llvm {
class Instruction;
class Type;
} // namespace llvm

namespace lotus::gpg {

using LocationId = std::uint32_t;
using StatementId = std::uint64_t;

struct Access {
  LocationId location = 0;
  IndirectionList indirections;
  bool upward_exposed = false;
  const llvm::Type *type = nullptr;
  bool k_limited = false;

  bool sameBase(const Access &other) const;
  bool sameSource(const Access &other) const;
  bool operator==(const Access &other) const;
  bool operator<(const Access &other) const;
};

enum class GPUKind : std::uint8_t {
  Assignment,
  Use,
  Parameter,
  Return,
  Boundary,
};

enum class QueryEndpoint : std::uint8_t {
  Source,
  Target,
};

struct GPUQuery {
  Access original;
  QueryEndpoint endpoint = QueryEndpoint::Target;

  bool operator==(const GPUQuery &other) const;
  bool operator<(const GPUQuery &other) const;
};

struct GPU {
  Access source;
  Access target;
  StatementId statement = 0;
  GPUKind kind = GPUKind::Assignment;
  const llvm::Instruction *origin = nullptr;
  bool source_may_alias_multiple = false;
  bool pointer_arithmetic = false;
  bool flow_insensitive = false;
  std::uint64_t provenance = 0;
  std::set<GPUQuery> queries;

  bool isBoundary() const { return kind == GPUKind::Boundary; }
  bool isUse() const { return kind == GPUKind::Use; }
  bool isIndirect() const { return source.indirections.size() > 1; }
  bool isPointsToEdge() const;
  std::string str() const;

  bool operator==(const GPU &other) const;
  bool operator<(const GPU &other) const;
};

using GPUSet = std::set<GPU>;
using AccessSet = std::set<Access>;

class GPUProducerIndex {
public:
  void add(const GPUSet &gpus);
  std::vector<const GPU *> candidates(const GPU &consumer) const;
  bool contains(const GPU &gpu) const;

private:
  using BaseKey = std::pair<LocationId, bool>;
  std::map<BaseKey, std::vector<const GPU *>> producers_;
};

enum class CompositionKind : std::uint8_t {
  TargetSource,
  SourceSource,
};

struct CompositionResult {
  GPUSet gpus;
  bool valid = false;
  bool desirable = false;
};

CompositionResult composeGPU(const GPU &consumer, const GPU &producer,
                             CompositionKind kind, unsigned k_limit = 0);

enum class Dependence : std::uint8_t {
  None = 0,
  ReadAfterWrite = 1,
  WriteAfterRead = 2,
  WriteAfterWrite = 4,
};

Dependence operator|(Dependence lhs, Dependence rhs);
bool hasDependence(Dependence value, Dependence kind);
Dependence definiteDependence(const GPU &consumer, const GPU &producer);

using TypeCompatibility =
    std::function<bool(const llvm::Type *, const llvm::Type *)>;

// Checks unresolved RaW/WaW aliasing from producer to consumer. Potential
// WaR is the reverse query and is deliberately excluded because a GPB's
// read-before-write semantics preserves it during coalescing.
bool potentialDependence(const GPU &consumer, const GPU &producer,
                         const TypeCompatibility &compatible);

struct ReductionResult {
  GPUSet reduced;
  GPUSet queued;
};

// Reduces a consumer maximally. `available` is the blocking-aware context;
// `unblocked` additionally contains producers hidden by barriers and is used
// to populate Queued as required by Appendix A.
ReductionResult reduceGPU(const GPU &consumer, const GPUSet &available,
                          const GPUSet &unblocked, unsigned k_limit = 0);
ReductionResult reduceGPU(const GPU &consumer,
                          const GPUProducerIndex &available,
                          const GPUProducerIndex &unblocked,
                          unsigned k_limit = 0);

} // namespace lotus::gpg
