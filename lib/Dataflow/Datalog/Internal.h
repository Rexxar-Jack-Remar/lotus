#pragma once

#include "Dataflow/Datalog/Datalog.h"

#include <any>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::datalog::internal {

using Row = std::vector<std::any>;

void combineHash(std::size_t &seed, std::size_t value);

struct RowHash {
  std::vector<ColumnType> columns;
  std::size_t operator()(const Row &row) const;
};

struct RowEqual {
  std::vector<ColumnType> columns;
  bool operator()(const Row &lhs, const Row &rhs) const;
};

struct KeyHash {
  std::vector<ColumnType> columns;
  std::size_t operator()(const Row &row) const;
};

struct KeyEqual {
  std::vector<ColumnType> columns;
  bool operator()(const Row &lhs, const Row &rhs) const;
};

class RuntimeIndex {
public:
  using BucketMap =
      std::unordered_map<Row, std::vector<std::size_t>, KeyHash, KeyEqual>;

  RuntimeIndex(ColumnMask mask, const std::vector<ColumnType> &all_columns);

  void rebuild(const std::vector<Row> &rows, std::size_t version);
  bool isCurrent(std::size_t version) const;
  const std::vector<std::size_t> *lookup(const Row &key) const;
  std::size_t entryCount() const;
  std::size_t approximateMemoryBytes() const;

private:
  static std::vector<ColumnType>
  selectedColumns(ColumnMask mask, const std::vector<ColumnType> &columns);
  Row keyFor(const Row &row) const;

  std::vector<std::size_t> columns_;
  BucketMap buckets_;
  std::size_t built_version_ = static_cast<std::size_t>(-1);
};

class RelationStorage {
public:
  using KeyMap = std::unordered_map<Row, std::size_t, KeyHash, KeyEqual>;

  explicit RelationStorage(RelationIR definition);

  const RelationIR &definition() const;
  const std::vector<Row> &rows() const;

  std::optional<Row> insertAndGetChanged(Row row);
  bool insert(Row row);
  bool contains(const Row &row) const;
  std::vector<Row> coalesce(std::vector<Row> candidates) const;
  std::size_t indexCount() const;
  std::size_t indexEntries() const;
  std::size_t indexMemoryBytes() const;

  void forEachMatching(ColumnMask mask, const Row &key, ExecutionStats &stats,
                       const std::function<void(const Row &)> &callback);

private:
  void validateRow(const Row &row) const;
  RuntimeIndex &getIndex(ColumnMask mask);
  Row latticeKey(const Row &row) const;

  RelationIR definition_;
  std::vector<Row> rows_;
  std::unordered_set<Row, RowHash, RowEqual> set_;
  std::unique_ptr<KeyMap> lattice_keys_;
  std::unordered_map<ColumnMask, std::unique_ptr<RuntimeIndex>> indices_;
  std::mutex index_mutex_;
  std::size_t version_ = 0;
};

struct VariableDefinition {
  std::string name;
  std::type_index type = typeid(void);
  bool anonymous = false;
};

struct PlannedSCC {
  std::vector<RelationId> relations;
  std::unordered_set<RelationId> relation_set;
  std::vector<std::size_t> rules;
  bool recursive = false;
  std::size_t stratum = 0;
};

struct ExecutionPlan {
  std::vector<RuleIR> rules;
  std::vector<PlannedSCC> sccs;
  std::size_t variable_count = 0;
  std::size_t planned_reorders = 0;
};

struct DependencyEdge {
  RelationId source = 0;
  RelationId target = 0;
  DependencyKind kind = DependencyKind::Positive;
};

std::vector<RuleIR> planAndValidateRules(
    const std::vector<RuleIR> &input_rules,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables,
    std::size_t &reorder_count);

std::vector<DependencyEdge>
collectDependencies(const std::vector<RuleIR> &rules);

std::vector<std::size_t>
computeStrata(const std::vector<DependencyEdge> &dependencies,
              std::size_t relation_count);

std::vector<PlannedSCC>
buildSCCPlan(const std::vector<RuleIR> &rules,
             const std::vector<DependencyEdge> &dependencies,
             const std::vector<std::size_t> &strata,
             std::size_t relation_count);

} // namespace lotus::datalog::internal
