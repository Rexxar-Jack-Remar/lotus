#include "Dataflow/Datalog/Internal.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lotus::datalog::internal {

void combineHash(std::size_t &seed, std::size_t value) {
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

std::size_t RowHash::operator()(const Row &row) const {
  std::size_t seed = 0;
  for (std::size_t i = 0; i < row.size(); ++i)
    combineHash(seed, columns[i].hash(row[i]));
  return seed;
}

bool RowEqual::operator()(const Row &lhs, const Row &rhs) const {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (!columns[i].equal(lhs[i], rhs[i]))
      return false;
  }
  return true;
}

std::size_t KeyHash::operator()(const Row &row) const {
  std::size_t seed = 0;
  for (std::size_t i = 0; i < row.size(); ++i)
    combineHash(seed, columns[i].hash(row[i]));
  return seed;
}

bool KeyEqual::operator()(const Row &lhs, const Row &rhs) const {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (!columns[i].equal(lhs[i], rhs[i]))
      return false;
  }
  return true;
}

RuntimeIndex::RuntimeIndex(ColumnMask mask,
                           const std::vector<ColumnType> &all_columns)
    : buckets_(0, KeyHash{selectedColumns(mask, all_columns)},
               KeyEqual{selectedColumns(mask, all_columns)}),
      unique_rows_(0, KeyHash{selectedColumns(mask, all_columns)},
                   KeyEqual{selectedColumns(mask, all_columns)}) {
  for (std::size_t i = 0; i < all_columns.size(); ++i) {
    if (mask & (ColumnMask{1} << i))
      columns_.push_back(i);
  }
  unique_ = columns_.size() == all_columns.size();
}

void RuntimeIndex::rebuild(const std::vector<Row> &rows, std::size_t version) {
  buckets_.clear();
  unique_rows_.clear();
  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    if (unique_)
      unique_rows_.emplace(keyFor(rows[row_index]), row_index);
    else
      buckets_[keyFor(rows[row_index])].push_back(row_index);
  }
  built_version_ = version;
}

bool RuntimeIndex::isCurrent(std::size_t version) const {
  return built_version_ == version;
}

bool RuntimeIndex::isUnique() const { return unique_; }

const std::vector<std::size_t> *RuntimeIndex::lookup(const Row &key) const {
  auto it = buckets_.find(key);
  return it == buckets_.end() ? nullptr : &it->second;
}

std::optional<std::size_t> RuntimeIndex::lookupUnique(const Row &key) const {
  auto it = unique_rows_.find(key);
  if (it == unique_rows_.end())
    return std::nullopt;
  return it->second;
}

std::size_t RuntimeIndex::bucketCount() const {
  return unique_ ? unique_rows_.size() : buckets_.size();
}

std::size_t RuntimeIndex::entryCount() const {
  if (unique_)
    return unique_rows_.size();
  std::size_t count = 0;
  for (const auto &[key, rows] : buckets_) {
    (void)key;
    count += rows.size();
  }
  return count;
}

std::size_t RuntimeIndex::approximateMemoryBytes() const {
  if (unique_) {
    std::size_t bytes = unique_rows_.bucket_count() * sizeof(void *);
    for (const auto &[key, row] : unique_rows_) {
      (void)row;
      bytes +=
          sizeof(key) + key.size() * sizeof(std::any) + sizeof(std::size_t);
    }
    return bytes;
  }
  std::size_t bytes = buckets_.bucket_count() * sizeof(void *);
  for (const auto &[key, rows] : buckets_) {
    bytes += sizeof(key) + key.size() * sizeof(std::any);
    bytes += sizeof(rows) + rows.capacity() * sizeof(std::size_t);
  }
  return bytes;
}

std::vector<ColumnType>
RuntimeIndex::selectedColumns(ColumnMask mask,
                              const std::vector<ColumnType> &columns) {
  std::vector<ColumnType> result;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (mask & (ColumnMask{1} << i))
      result.push_back(columns[i]);
  }
  return result;
}

Row RuntimeIndex::keyFor(const Row &row) const {
  Row key;
  key.reserve(columns_.size());
  for (std::size_t column : columns_)
    key.push_back(row[column]);
  return key;
}

RelationStorage::RelationStorage(RelationIR definition)
    : definition_(std::move(definition)),
      set_(0, RowHash{definition_.columns}, RowEqual{definition_.columns}) {
  if (definition_.kind == RelationKind::Lattice) {
    std::vector<ColumnType> key_columns(definition_.columns.begin(),
                                        definition_.columns.end() - 1);
    lattice_keys_ = std::make_unique<KeyMap>(0, KeyHash{key_columns},
                                             KeyEqual{key_columns});
  }
}

const RelationIR &RelationStorage::definition() const { return definition_; }

const std::vector<Row> &RelationStorage::rows() const { return rows_; }

std::optional<Row> RelationStorage::insertAndGetChanged(Row row) {
  validateRow(row);
  if (definition_.kind == RelationKind::Lattice) {
    Row key = latticeKey(row);
    auto key_it = lattice_keys_->find(key);
    if (key_it == lattice_keys_->end()) {
      const std::size_t row_index = rows_.size();
      rows_.push_back(std::move(row));
      lattice_keys_->emplace(std::move(key), row_index);
      ++version_;
      return rows_.back();
    }

    Row &current = rows_[key_it->second];
    if (!definition_.lattice_join(current.back(), row.back()))
      return std::nullopt;
    ++version_;
    return current;
  }

  if (!set_.insert(row).second)
    return std::nullopt;
  rows_.push_back(std::move(row));
  ++version_;
  return rows_.back();
}

bool RelationStorage::insert(Row row) {
  return insertAndGetChanged(std::move(row)).has_value();
}

bool RelationStorage::contains(const Row &row) const {
  validateRow(row);
  if (definition_.kind == RelationKind::Lattice) {
    auto it = lattice_keys_->find(latticeKey(row));
    if (it == lattice_keys_->end())
      return false;
    return definition_.columns.back().equal(rows_[it->second].back(),
                                            row.back());
  }
  return set_.find(row) != set_.end();
}

std::vector<Row> RelationStorage::coalesce(std::vector<Row> candidates) const {
  if (definition_.kind != RelationKind::Lattice) {
    std::unordered_set<Row, RowHash, RowEqual> unique(
        0, RowHash{definition_.columns}, RowEqual{definition_.columns});
    std::vector<Row> result;
    result.reserve(candidates.size());
    for (Row &candidate : candidates) {
      validateRow(candidate);
      if (unique.insert(candidate).second)
        result.push_back(std::move(candidate));
    }
    return result;
  }

  std::vector<ColumnType> key_columns(definition_.columns.begin(),
                                      definition_.columns.end() - 1);
  KeyMap keys(0, KeyHash{key_columns}, KeyEqual{key_columns});
  std::vector<Row> result;
  for (Row &candidate : candidates) {
    validateRow(candidate);
    Row key = latticeKey(candidate);
    auto [it, inserted] = keys.emplace(std::move(key), result.size());
    if (inserted)
      result.push_back(std::move(candidate));
    else
      definition_.lattice_join(result[it->second].back(), candidate.back());
  }
  return result;
}

std::size_t RelationStorage::candidateHash(const Row &row) const {
  validateRow(row);
  const std::size_t column_count =
      definition_.kind == RelationKind::Lattice ? row.size() - 1 : row.size();
  std::size_t seed = 0;
  for (std::size_t column = 0; column < column_count; ++column)
    combineHash(seed, definition_.columns[column].hash(row[column]));
  return seed;
}

RelationStorage::BatchMergeResult
RelationStorage::mergeCoalesced(std::vector<Row> candidates,
                                Scheduler &scheduler, std::size_t grain_size) {
  BatchMergeResult result;
  if (candidates.empty())
    return result;

  for (const Row &candidate : candidates)
    validateRow(candidate);

  const std::size_t grain = std::max<std::size_t>(1, grain_size);
  const std::size_t task_count = std::min(
      scheduler.workerCount(), (candidates.size() + grain - 1) / grain);
  std::vector<unsigned char> changed(candidates.size(), 0);
  std::vector<std::optional<std::size_t>> lattice_rows(candidates.size());

  auto inspect = [&](std::size_t task) {
    const std::size_t begin = candidates.size() * task / task_count;
    const std::size_t end = candidates.size() * (task + 1) / task_count;
    if (definition_.kind == RelationKind::Set) {
      for (std::size_t index = begin; index < end; ++index)
        changed[index] = set_.find(candidates[index]) == set_.end();
      return;
    }

    for (std::size_t index = begin; index < end; ++index) {
      auto found = lattice_keys_->find(latticeKey(candidates[index]));
      if (found == lattice_keys_->end()) {
        changed[index] = 1;
        continue;
      }
      lattice_rows[index] = found->second;
      changed[index] = definition_.lattice_join(rows_[found->second].back(),
                                                candidates[index].back());
    }
  };

  if (task_count > 1) {
    scheduler.parallelFor(task_count, inspect);
    result.parallel_tasks += task_count;
  } else {
    inspect(0);
  }

  result.changed.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (!changed[index])
      continue;
    if (definition_.kind == RelationKind::Set) {
      set_.insert(candidates[index]);
      rows_.push_back(std::move(candidates[index]));
      result.changed.push_back(rows_.back());
      continue;
    }
    if (lattice_rows[index]) {
      result.changed.push_back(rows_[*lattice_rows[index]]);
      continue;
    }
    const std::size_t row_index = rows_.size();
    Row key = latticeKey(candidates[index]);
    rows_.push_back(std::move(candidates[index]));
    lattice_keys_->emplace(std::move(key), row_index);
    result.changed.push_back(rows_.back());
  }
  version_ += result.changed.size();
  return result;
}

std::size_t RelationStorage::estimatedLookupCardinality(ColumnMask mask) {
  if (mask == 0)
    return std::max<std::size_t>(1, rows_.size());
  std::lock_guard<std::mutex> lock(index_mutex_);
  RuntimeIndex &index = getIndex(mask);
  if (!index.isCurrent(version_))
    index.rebuild(rows_, version_);
  const std::size_t distinct = index.bucketCount();
  if (distinct == 0)
    return 1;
  return std::max<std::size_t>(1, (rows_.size() + distinct - 1) / distinct);
}

std::size_t RelationStorage::indexCount() const { return indices_.size(); }

std::size_t RelationStorage::indexEntries() const {
  std::size_t count = 0;
  for (const auto &[mask, index] : indices_) {
    (void)mask;
    count += index->entryCount();
  }
  return count;
}

std::size_t RelationStorage::indexMemoryBytes() const {
  std::size_t bytes = 0;
  for (const auto &[mask, index] : indices_) {
    (void)mask;
    bytes += index->approximateMemoryBytes();
  }
  return bytes;
}

void RelationStorage::forEachMatching(
    ColumnMask mask, const Row &key, ExecutionStats &stats,
    const std::function<void(const Row &)> &callback) {
  for (const Row *row : matchingRows(mask, key, stats))
    callback(*row);
}

std::vector<const Row *> RelationStorage::matchingRows(ColumnMask mask,
                                                       const Row &key,
                                                       ExecutionStats &stats) {
  std::vector<const Row *> result;
  if (mask == 0) {
    result.reserve(rows_.size());
    for (const Row &row : rows_) {
      ++stats.tuples_scanned;
      result.push_back(&row);
    }
    return result;
  }

  ++stats.index_lookups;
  const std::vector<std::size_t> *matches = nullptr;
  std::optional<std::size_t> unique_match;
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    RuntimeIndex &index = getIndex(mask);
    if (!index.isCurrent(version_))
      index.rebuild(rows_, version_);
    if (index.isUnique())
      unique_match = index.lookupUnique(key);
    else
      matches = index.lookup(key);
  }
  if (unique_match) {
    ++stats.tuples_scanned;
    result.push_back(&rows_[*unique_match]);
    return result;
  }
  if (!matches)
    return result;

  // Relation rows and indexes are immutable during an evaluation epoch, so the
  // bucket remains valid after releasing the build mutex.
  result.reserve(matches->size());
  for (std::size_t row_index : *matches) {
    ++stats.tuples_scanned;
    result.push_back(&rows_[row_index]);
  }
  return result;
}

void RelationStorage::validateRow(const Row &row) const {
  if (row.size() != definition_.columns.size())
    throw std::invalid_argument("fact arity does not match relation '" +
                                definition_.name + "'");
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (std::type_index(row[i].type()) != definition_.columns[i].type) {
      throw std::invalid_argument("fact column type does not match relation '" +
                                  definition_.name + "'");
    }
  }
}

RuntimeIndex &RelationStorage::getIndex(ColumnMask mask) {
  auto it = indices_.find(mask);
  if (it == indices_.end()) {
    it = indices_
             .emplace(mask,
                      std::make_unique<RuntimeIndex>(mask, definition_.columns))
             .first;
  }
  return *it->second;
}

Row RelationStorage::latticeKey(const Row &row) const {
  return Row(row.begin(), row.end() - 1);
}

} // namespace lotus::datalog::internal
