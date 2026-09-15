#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lotus {
namespace bootstrap {

using Id = std::uint32_t;
constexpr Id INVALID = std::numeric_limits<Id>::max();
constexpr Id UNKNOWN = 0;
constexpr Id NULL_OBJECT = 1;

// The empty set is bottom; {UNKNOWN} is top, not an ordinary allocation site.
class PointsToSet {
public:
  class const_iterator {
  public:
    using value_type = Id;
    using difference_type = std::ptrdiff_t;
    using pointer = const Id *;
    using reference = Id;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() = default;
    Id operator*() const;
    const_iterator &operator++();
    bool operator==(const const_iterator &other) const;
    bool operator!=(const const_iterator &other) const {
      return !(*this == other);
    }

  private:
    friend class PointsToSet;
    const_iterator(const PointsToSet *set, std::size_t word,
                   std::uint64_t remaining);
    const PointsToSet *m_set = nullptr;
    std::size_t m_word = 0;
    std::uint64_t m_remaining = 0;
  };

  class ObjectRange {
  public:
    explicit ObjectRange(const PointsToSet &set) : m_set(&set) {}
    const_iterator begin() const;
    const_iterator end() const;

  private:
    const PointsToSet *m_set;
  };

  PointsToSet() = default;
  explicit PointsToSet(Id object) { insert(object); }
  static PointsToSet top() { return PointsToSet(UNKNOWN); }
  bool isTop() const;
  bool empty() const { return m_words.empty(); }
  std::size_t size() const;
  bool contains(Id object) const;
  bool insert(Id object);
  bool join(const PointsToSet &other);
  bool intersects(const PointsToSet &other) const;
  ObjectRange objects() const { return ObjectRange(*this); }
  const_iterator begin() const;
  const_iterator end() const;
  bool operator==(const PointsToSet &rhs) const;
  bool operator!=(const PointsToSet &rhs) const { return !(*this == rhs); }
  bool operator<(const PointsToSet &rhs) const;

private:
  struct Word {
    Id index = 0;
    std::uint64_t bits = 0;
    bool operator==(const Word &other) const {
      return index == other.index && bits == other.bits;
    }
  };
  static constexpr unsigned WORD_BITS = 64;
  std::vector<Word> m_words;
};

enum class ObjectKind { Unknown, Null, Global, Stack, Heap, Function };
struct Object {
  std::string name;
  ObjectKind kind = ObjectKind::Heap;
  // True only for one concrete, whole pointer cell. Never true for summary
  // heaps, aggregate objects, loop allocations, or recursive-frame allocations.
  bool singleton = false;
  std::uint64_t bytes = 0; // 0 means a normalized, indivisible pointer cell.
  PointsToSet initial = PointsToSet::top();
  Id function = INVALID;
};
struct Value {
  std::string name;
  Id function = INVALID;
  bool constant = false;
  PointsToSet initial;
};

enum class Opcode {
  Nop,
  Address,
  Allocate,
  Copy,
  Join,
  Load,
  Store,
  Havoc,
  Unknown,
  Call,
  Return
};
struct Instruction {
  Id id = INVALID;
  Opcode opcode = Opcode::Nop;
  Id result = INVALID;
  // Copy/Join/Load: sources. Store: {address, value}. Havoc: optional address.
  // Call: positional actual arguments, including INVALID for non-pointer args.
  std::vector<Id> operands;
  Id object = INVALID;
  bool may_be_null = false; // Allocation failure is not a fresh object.
  Id callee = INVALID;      // Direct callee, otherwise indirect_target.
  Id indirect_target = INVALID;
  bool opaque_alternative = false; // EH/callbr: conservative additional path.
  bool writes_memory = true;       // Honored for opaque/external calls.
  std::uint64_t write_bytes = 0;
  std::uint64_t read_bytes = 0;
};
struct Edge {
  Id target = INVALID;
  // All assignments on an edge are simultaneous (LLVM PHI semantics).
  std::vector<std::pair<Id, Id>> phi;
};
struct Block {
  std::vector<Instruction> instructions;
  std::vector<Edge> successors;
};
struct Function {
  std::string name;
  std::vector<Id> parameters;
  std::vector<Block> blocks;
  bool external = false;
  bool writes_memory = true;
};

// Small LLVM-independent IR. All IDs are stable vector indices except site IDs,
// which are assigned by append(). Finish construction before creating Analysis.
struct Program {
  Program();
  std::vector<Object> objects;
  std::vector<Value> values;
  std::vector<Function> functions;
  Id entry = INVALID;
  Id addObject(Object object);
  Id addValue(Value value);
  Id addConstant(PointsToSet points_to, std::string name = {});
  Id addFunction(std::string name, bool external = false);
  Id addBlock(Id function);
  Id append(Id function, Id block, Instruction instruction);
  void validate() const;

private:
  Id m_next_site = 0;
};

struct Options {
  std::size_t andersen_threshold = 60;
  bool adaptive_andersen_threshold = true;
  // Zero disables this adaptive preprocessing cost guard.
  std::size_t max_andersen_partition_size = 4096;
  // Estimated partition-size x hierarchy-node budget; zero is unlimited.
  std::size_t max_andersen_work = 4 * 1024 * 1024;
  bool parallel_clusters = true;
  // Zero selects hardware_concurrency. Values above one enable parallel work.
  std::size_t parallelism = 0;
  // Resource exhaustion yields top plus an explicit status, never partial sets.
  std::size_t max_contexts = 4096;
  std::size_t max_steps = 1000000;
  bool enable_slicing = true; // Useful for differential testing.
  bool enable_clustering = true;
};

struct SteensgaardHierarchy {
  // Dense component IDs for normalized values and allocation objects.
  std::vector<Id> value_components;
  std::vector<Id> object_components;
  // Cycle-collapsed points-to hierarchy. Edges run from a pointer partition
  // to the partition reached by one dereference.
  std::vector<std::vector<Id>> successors;
  std::vector<std::vector<Id>> predecessors;
  std::vector<std::vector<Id>> values;
  std::vector<std::size_t> depth;
  std::vector<bool> cyclic;
  Id top_component = INVALID;

  bool isHigher(Id higher, Id lower) const;
};

struct Statistics {
  std::size_t steensgaard_partitions = 0;
  std::size_t steensgaard_largest_partition = 0;
  std::vector<std::size_t> steensgaard_partition_sizes;
  std::size_t hierarchy_nodes = 0;
  std::size_t hierarchy_edges = 0;
  std::size_t hierarchy_max_depth = 0;
  std::size_t hierarchy_cyclic_components = 0;
  std::size_t andersen_runs = 0;
  std::size_t adaptive_refinement_skips = 0;
  std::size_t adaptive_refinement_rejections = 0;
  std::size_t adaptive_cost_skips = 0;
  std::size_t effective_andersen_threshold = 0;
  std::size_t clusters = 0;
  std::size_t largest_cluster = 0;
  std::vector<std::size_t> cluster_sizes;
  std::size_t cover_memberships = 0;
  std::size_t overlapping_values = 0;
  std::size_t maximum_cover_memberships = 0;
  std::size_t slice_values = 0;
  std::size_t slice_objects = 0;
  std::size_t evaluated_clusters = 0;
  std::size_t parallel_cluster_tasks = 0;
  std::size_t call_graph_sccs = 0;
  std::size_t recursive_call_graph_sccs = 0;
  std::size_t scc_reschedules = 0;
  std::size_t contexts = 0;
  std::size_t context_cache_hits = 0;
  std::size_t context_cache_misses = 0;
  std::size_t maximum_contexts_per_cluster = 0;
  std::size_t summary_updates = 0;
  double preprocessing_milliseconds = 0.0;
  std::vector<double> cluster_solve_milliseconds;
  std::size_t steps = 0;
  std::size_t fallback_queries = 0;
};

enum class QueryStatus { Complete, Unreachable, ResourceLimit };
enum class Point { Before, After };
struct QueryResult {
  PointsToSet points_to;
  QueryStatus status = QueryStatus::Unreachable;
  bool reachable() const { return status != QueryStatus::Unreachable; }
};

// Context is a complete sequence of call-site IDs starting in Program::entry.
// An empty context queries the entry activation, not a union over all callers.
using Context = std::vector<Id>;
class Analysis {
public:
  explicit Analysis(const Program &program, Options options = {});
  ~Analysis();
  Analysis(Analysis &&) noexcept;
  Analysis &operator=(Analysis &&) noexcept;
  Analysis(const Analysis &) = delete;
  Analysis &operator=(const Analysis &) = delete;

  QueryResult pointsTo(Id value, Id site, const Context &context = {},
                       Point point = Point::Before);
  QueryResult pointsToAllContexts(Id value, Id site,
                                  Point point = Point::Before);
  // Eagerly construct every cluster solver, using configured parallelism.
  void precomputeAll();
  // Conservative query helper. Unreachable is not used to prove disjointness.
  bool mayAlias(Id lhs, Id rhs, Id site, const Context &context = {});
  const Statistics &statistics() const;
  const std::vector<std::vector<Id>> &clusters() const;
  const SteensgaardHierarchy &hierarchy() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace bootstrap
} // namespace lotus
