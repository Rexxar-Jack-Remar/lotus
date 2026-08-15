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
                           const std::vector<ColumnType> &all_columns) {
  for (std::size_t i = 0; i < all_columns.size(); ++i) {
    if (mask & (ColumnMask{1} << i)) {
      columns_.push_back(i);
      column_types_.push_back(all_columns[i]);
    }
  }
}

std::size_t RuntimeIndex::hash(const Row &row) const {
  std::size_t seed = 0;
  for (std::size_t index = 0; index < columns_.size(); ++index)
    combineHash(seed, column_types_[index].hash(row[columns_[index]]));
  return seed;
}

std::size_t RuntimeIndex::hash(const KeyView &key) const {
  if (key.size != columns_.size())
    throw std::logic_error("Datalog lookup key does not match index mask");
  std::size_t seed = 0;
  for (std::size_t index = 0; index < key.size; ++index)
    combineHash(seed, column_types_[index].hash(*key.values[index]));
  return seed;
}

void RuntimeIndex::insert(const std::vector<Row> &rows, std::size_t row_index) {
  buckets_[hash(rows[row_index])].push_back(row_index);
}

void RuntimeIndex::erase(std::size_t key_hash, std::size_t row_index) {
  auto bucket = buckets_.find(key_hash);
  if (bucket == buckets_.end())
    throw std::logic_error("Datalog index lost an indexed row");
  auto position =
      std::find(bucket->second.begin(), bucket->second.end(), row_index);
  if (position == bucket->second.end())
    throw std::logic_error("Datalog index lost an indexed row");
  bucket->second.erase(position);
  if (bucket->second.empty())
    buckets_.erase(bucket);
}

void RuntimeIndex::rebuild(const std::vector<Row> &rows, std::size_t version) {
  buckets_.clear();
  buckets_.reserve(rows.size());
  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index)
    insert(rows, row_index);
  built_version_ = version;
}

void RuntimeIndex::append(const std::vector<Row> &rows, std::size_t row_index,
                          std::size_t version) {
  if (!isCurrent(version - 1))
    return;
  insert(rows, row_index);
  built_version_ = version;
}

void RuntimeIndex::update(const std::vector<Row> &rows, std::size_t row_index,
                          const Row &previous_row, std::size_t version) {
  if (!isCurrent(version - 1))
    return;
  const std::size_t old_hash = hash(previous_row);
  const std::size_t new_hash = hash(rows[row_index]);
  if (old_hash != new_hash) {
    erase(old_hash, row_index);
    buckets_[new_hash].push_back(row_index);
  }
  built_version_ = version;
}

bool RuntimeIndex::isCurrent(std::size_t version) const {
  return built_version_ == version;
}

const std::vector<std::size_t> *RuntimeIndex::lookup(const KeyView &key) const {
  auto it = buckets_.find(hash(key));
  return it == buckets_.end() ? nullptr : &it->second;
}

bool RuntimeIndex::matches(const Row &row, const KeyView &key) const {
  if (key.size != columns_.size())
    return false;
  for (std::size_t index = 0; index < key.size; ++index) {
    if (!column_types_[index].equal(row[columns_[index]], *key.values[index]))
      return false;
  }
  return true;
}

std::size_t RuntimeIndex::bucketCount() const { return buckets_.size(); }

std::size_t RuntimeIndex::entryCount() const {
  std::size_t count = 0;
  for (const auto &[key, rows] : buckets_) {
    (void)key;
    count += rows.size();
  }
  return count;
}

std::size_t RuntimeIndex::approximateMemoryBytes() const {
  std::size_t bytes = buckets_.bucket_count() * sizeof(void *);
  for (const auto &[key, rows] : buckets_) {
    (void)key;
    bytes += sizeof(key) + sizeof(rows) + rows.capacity() * sizeof(std::size_t);
  }
  return bytes;
}

RelationStorage::RelationStorage(RelationIR definition)
    : definition_(std::move(definition)),
      set_(0, RowHash{definition_.columns}, RowEqual{definition_.columns}),
      base_set_(0, RowHash{definition_.columns},
                RowEqual{definition_.columns}) {
  if (definition_.kind == RelationKind::Lattice) {
    std::vector<ColumnType> key_columns(definition_.columns.begin(),
                                        definition_.columns.end() - 1);
    lattice_keys_ = std::make_unique<KeyMap>(0, KeyHash{key_columns},
                                             KeyEqual{key_columns});
    base_lattice_keys_ = std::make_unique<KeyMap>(0, KeyHash{key_columns},
                                                  KeyEqual{key_columns});
  }
}

const RelationIR &RelationStorage::definition() const { return definition_; }

const std::vector<Row> &RelationStorage::rows() const { return rows_; }

void RelationStorage::appendRow(Row row) {
  const std::size_t row_index = rows_.size();
  rows_.push_back(std::move(row));
  ++version_;
  std::lock_guard<std::mutex> lock(index_mutex_);
  for (auto &[mask, index] : indices_) {
    (void)mask;
    index->append(rows_, row_index, version_);
  }
}

void RelationStorage::updateRow(std::size_t row_index, Row row) {
  Row previous_row = rows_[row_index];
  rows_[row_index] = std::move(row);
  ++version_;
  std::lock_guard<std::mutex> lock(index_mutex_);
  for (auto &[mask, index] : indices_) {
    (void)mask;
    index->update(rows_, row_index, previous_row, version_);
  }
}

bool RelationStorage::insertBase(Row row) {
  validateRow(row);
  if (definition_.kind == RelationKind::Set) {
    if (!base_set_.insert(row).second)
      return false;
    base_rows_.push_back(row);
    ++base_version_;
    if (set_.insert(row).second)
      appendRow(std::move(row));
    return true;
  }

  Row key = latticeKey(row);
  auto base_found = base_lattice_keys_->find(key);
  if (base_found == base_lattice_keys_->end()) {
    const std::size_t base_index = base_rows_.size();
    base_rows_.push_back(row);
    base_lattice_keys_->emplace(std::move(key), base_index);
    ++base_version_;
  } else {
    Row proposed = base_rows_[base_found->second];
    if (!definition_.lattice_join(proposed.back(), row.back()))
      return false;
    base_rows_[base_found->second] = std::move(proposed);
    ++base_version_;
  }

  const Row &base_row =
      base_rows_[base_lattice_keys_->find(latticeKey(row))->second];
  auto total_found = lattice_keys_->find(latticeKey(base_row));
  if (total_found == lattice_keys_->end()) {
    const std::size_t total_index = rows_.size();
    lattice_keys_->emplace(latticeKey(base_row), total_index);
    appendRow(base_row);
    return true;
  }

  Row proposed = rows_[total_found->second];
  if (definition_.lattice_join(proposed.back(), base_row.back()))
    updateRow(total_found->second, std::move(proposed));
  return true;
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
    if (inserted) {
      result.push_back(std::move(candidate));
      continue;
    }
    Row proposed = result[it->second];
    if (definition_.lattice_join(proposed.back(), candidate.back()))
      result[it->second] = std::move(proposed);
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

bool RelationStorage::rowsEqual(const Row &lhs, const Row &rhs) const {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t column = 0; column < lhs.size(); ++column) {
    if (!definition_.columns[column].equal(lhs[column], rhs[column]))
      return false;
  }
  return true;
}

RelationStorage::BatchMergeResult RelationStorage::mergeDerivedCoalesced(
    std::vector<Row> candidates, Scheduler &scheduler, std::size_t grain_size) {
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
  std::vector<std::optional<Row>> proposals(candidates.size());

  // Inspection deliberately works on copies.  A user lattice operation may
  // throw; no live row or index may change unless every inspect task succeeds.
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
      Row proposed = rows_[found->second];
      if (definition_.lattice_join(proposed.back(), candidates[index].back())) {
        changed[index] = 1;
        proposals[index] = std::move(proposed);
      }
    }
  };

  if (task_count > 1) {
    scheduler.parallelFor(task_count, inspect);
    result.parallel_tasks += task_count;
  } else {
    inspect(0);
  }

  result.changed_row_ids.reserve(candidates.size());
  result.changed_lattice_rows.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (!changed[index])
      continue;
    if (definition_.kind == RelationKind::Set) {
      if (set_.insert(candidates[index]).second) {
        appendRow(std::move(candidates[index]));
        has_derived_state_ = true;
        result.changed_row_ids.push_back(rows_.size() - 1);
      }
      continue;
    }
    if (lattice_rows[index]) {
      updateRow(*lattice_rows[index], std::move(*proposals[index]));
      has_derived_state_ = true;
      result.changed_lattice_rows.push_back(rows_[*lattice_rows[index]]);
      continue;
    }
    const std::size_t row_index = rows_.size();
    lattice_keys_->emplace(latticeKey(candidates[index]), row_index);
    appendRow(std::move(candidates[index]));
    has_derived_state_ = true;
    result.changed_lattice_rows.push_back(rows_.back());
  }
  return result;
}

void RelationStorage::rebuildFromBase() {
  rows_ = base_rows_;
  set_.clear();
  if (definition_.kind == RelationKind::Set) {
    set_.reserve(rows_.size());
    for (const Row &row : rows_)
      set_.insert(row);
  } else {
    lattice_keys_->clear();
    lattice_keys_->reserve(rows_.size());
    for (std::size_t row_index = 0; row_index < rows_.size(); ++row_index)
      lattice_keys_->emplace(latticeKey(rows_[row_index]), row_index);
  }

  ++version_;
  std::lock_guard<std::mutex> lock(index_mutex_);
  for (auto &[mask, index] : indices_) {
    (void)mask;
    index->rebuild(rows_, version_);
  }
}

void RelationStorage::discardDerived() {
  if (!has_derived_state_)
    return;
  has_derived_state_ = false;
  rebuildFromBase();
}

std::size_t RelationStorage::baseVersion() const { return base_version_; }

std::size_t RelationStorage::estimatedLookupCardinality(ColumnMask mask) const {
  if (mask == 0)
    return std::max<std::size_t>(1, rows_.size());

  std::lock_guard<std::mutex> lock(statistics_mutex_);
  auto cached = lookup_estimates_.find(mask);
  if (cached != lookup_estimates_.end() && cached->second.first == version_)
    return cached->second.second;

  std::vector<ColumnType> key_columns;
  for (std::size_t column = 0; column < definition_.columns.size(); ++column) {
    if (mask & (ColumnMask{1} << column))
      key_columns.push_back(definition_.columns[column]);
  }
  std::unordered_set<Row, KeyHash, KeyEqual> distinct(0, KeyHash{key_columns},
                                                      KeyEqual{key_columns});
  for (const Row &row : rows_) {
    Row key;
    key.reserve(key_columns.size());
    for (std::size_t column = 0; column < definition_.columns.size();
         ++column) {
      if (mask & (ColumnMask{1} << column))
        key.push_back(row[column]);
    }
    distinct.insert(std::move(key));
  }
  const std::size_t estimate =
      distinct.empty()
          ? 1
          : std::max<std::size_t>(1, (rows_.size() + distinct.size() - 1) /
                                         distinct.size());
  lookup_estimates_[mask] = {version_, estimate};
  return estimate;
}

void RelationStorage::ensureIndex(ColumnMask mask) {
  if (mask == 0)
    return;
  std::lock_guard<std::mutex> lock(index_mutex_);
  RuntimeIndex &index = getIndex(mask);
  if (!index.isCurrent(version_))
    index.rebuild(rows_, version_);
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
    const KeyView &key, ExecutionStats &stats,
    const std::function<void(const Row &)> &callback) {
  if (key.mask == 0) {
    for (const Row &row : rows_) {
      ++stats.tuples_scanned;
      callback(row);
    }
    return;
  }

  ++stats.index_lookups;
  const std::vector<std::size_t> *matches = nullptr;
  RuntimeIndex *index = nullptr;
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    index = &getIndex(key.mask);
    if (!index->isCurrent(version_))
      index->rebuild(rows_, version_);
    matches = index->lookup(key);
  }
  if (!matches)
    return;
  for (std::size_t row_index : *matches) {
    const Row &row = rows_[row_index];
    if (!index->matches(row, key))
      continue;
    ++stats.tuples_scanned;
    callback(row);
  }
}

std::size_t RelationStorage::matchingCandidateCount(const KeyView &key,
                                                    ExecutionStats &stats) {
  if (key.mask == 0)
    return rows_.size();
  ++stats.index_lookups;
  std::lock_guard<std::mutex> lock(index_mutex_);
  RuntimeIndex &index = getIndex(key.mask);
  if (!index.isCurrent(version_))
    index.rebuild(rows_, version_);
  const std::vector<std::size_t> *matches = index.lookup(key);
  return matches ? matches->size() : 0;
}

void RelationStorage::forEachMatchingSlice(
    const KeyView &key, std::size_t begin, std::size_t end,
    const std::function<void(const Row &)> &callback) {
  if (key.mask == 0) {
    const std::size_t bounded_end = std::min(end, rows_.size());
    for (std::size_t row_index = begin; row_index < bounded_end; ++row_index)
      callback(rows_[row_index]);
    return;
  }

  const std::vector<std::size_t> *matches = nullptr;
  RuntimeIndex *index = nullptr;
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    index = &getIndex(key.mask);
    if (!index->isCurrent(version_))
      index->rebuild(rows_, version_);
    matches = index->lookup(key);
  }
  if (!matches)
    return;
  const std::size_t bounded_end = std::min(end, matches->size());
  for (std::size_t match_index = begin; match_index < bounded_end;
       ++match_index) {
    const Row &row = rows_[(*matches)[match_index]];
    if (index->matches(row, key))
      callback(row);
  }
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
