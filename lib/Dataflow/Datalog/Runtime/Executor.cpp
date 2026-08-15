#include "Dataflow/Datalog/ContextInternal.h"
#include "Dataflow/Datalog/Internal.h"
#include "Dataflow/Datalog/Program.h"

#include <algorithm>
#include <any>
#include <array>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::datalog {

using internal::AtomPlan;
using internal::buildSCCPlan;
using internal::collectDependencies;
using internal::computeStrata;
using internal::DependencyEdge;
using internal::ExecutionPlan;
using internal::HeadTermPlan;
using internal::KeyView;
using internal::OpCode;
using internal::PhysicalOp;
using internal::planAndValidateRules;
using internal::PlannedSCC;
using internal::RelationStorage;
using internal::Row;
using internal::RulePlan;

struct CompiledProgram::Impl {
  std::shared_ptr<Context::Impl> context;
  ExecutionPlan plan;
  ExecutionStats stats;
  struct ExecutionContext {
    const ExecutionOptions &options;
  };
  ExecutionContext *execution = nullptr;
  std::vector<std::size_t> completed_base_versions;
  bool has_completed_run = false;
  std::mutex trace_mutex;

  struct ExecutionScope {
    Impl &impl;
    std::unique_lock<std::mutex> context_lock;
    explicit ExecutionScope(Impl &impl, ExecutionContext &execution)
        : impl(impl), context_lock(impl.context->execution_mutex) {
      impl.context->running = true;
      impl.execution = &execution;
    }
    ~ExecutionScope() {
      impl.execution = nullptr;
      impl.context->running = false;
    }
  };

  const ExecutionOptions &options() const {
    if (!execution)
      throw std::logic_error("Datalog execution state is not active");
    return execution->options;
  }

  RelationStorage &relation(RelationId id) {
    return *context->relations.at(id);
  }

  using CandidateMap = std::unordered_map<RelationId, std::vector<Row>>;
  using DeltaMap = std::unordered_map<RelationId, std::vector<Row>>;
  using DeltaHashMap = std::unordered_map<
      RelationId, std::unordered_map<std::size_t, std::vector<std::size_t>>>;

  struct DeltaView {
    const std::vector<Row> *rows = nullptr;
    std::size_t begin = 0;
    std::size_t end = 0;
  };

  struct EvaluationTask {
    std::size_t rule_index = 0;
    std::optional<std::size_t> delta_item;
    std::optional<DeltaView> delta;
  };

  static void mergeStats(ExecutionStats &destination,
                         const ExecutionStats &source) {
    destination.rule_evaluations += source.rule_evaluations;
    destination.tuples_scanned += source.tuples_scanned;
    destination.index_lookups += source.index_lookups;
    destination.parallel_tasks += source.parallel_tasks;
    destination.parallel_rule_tasks += source.parallel_rule_tasks;
    destination.parallel_merge_tasks += source.parallel_merge_tasks;
    destination.parallel_aggregate_tasks += source.parallel_aggregate_tasks;
  }

  void collectStorageStats() {
    for (const auto &storage : context->relations) {
      stats.total_facts += storage->rows().size();
      stats.index_count += storage->indexCount();
      stats.index_entries += storage->indexEntries();
      stats.index_memory_bytes += storage->indexMemoryBytes();
    }
  }

  std::ostream &traceStream() {
    if (execution && options().trace_stream)
      return *options().trace_stream;
    return std::cerr;
  }

  void traceRule(std::size_t rule_index,
                 std::optional<std::size_t> delta_item) {
    if (!execution || !options().trace_rule)
      return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    const RulePlan &rule = plan.rules[rule_index];
    traceStream() << "rule " << rule_index << " -> "
                  << relation(rule.head_relation).definition().name;
    if (delta_item)
      traceStream() << " delta-body=" << *delta_item;
    traceStream() << '\n';
  }

  void traceDelta(std::size_t iteration, const DeltaMap &delta) {
    if (!execution || !options().trace_delta)
      return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    traceStream() << "iteration " << iteration << ':';
    for (const auto &[relation_id, rows] : delta) {
      traceStream() << ' ' << relation(relation_id).definition().name << '='
                    << rows.size();
    }
    traceStream() << '\n';
  }

  bool matchRow(const AtomPlan &atom, const Row &row, Binding &binding,
                const std::function<void()> &continuation) {
    std::array<VarId, KeyView::MAX_COLUMNS> newly_bound{};
    std::size_t newly_bound_count = 0;
    for (std::size_t column = 0; column < atom.terms.size(); ++column) {
      const auto &term = atom.terms[column];
      const ColumnType &type =
          relation(atom.relation).definition().columns[column];
      if (!term.is_variable) {
        if (!type.equal(term.constant, row[column])) {
          for (std::size_t index = 0; index < newly_bound_count; ++index)
            binding[newly_bound[index]].reset();
          return false;
        }
        continue;
      }

      if (binding[term.variable]) {
        if (!type.equal(*binding[term.variable], row[column])) {
          for (std::size_t index = 0; index < newly_bound_count; ++index)
            binding[newly_bound[index]].reset();
          return false;
        }
      } else {
        binding[term.variable].bindReference(row[column]);
        newly_bound[newly_bound_count++] = term.variable;
      }
    }

    continuation();
    for (std::size_t index = 0; index < newly_bound_count; ++index)
      binding[newly_bound[index]].reset();
    return true;
  }

  KeyView lookupKey(const AtomPlan &atom, const Binding &binding) {
    KeyView key;
    key.mask = atom.lookup_mask;
    for (std::size_t column = 0; column < atom.terms.size(); ++column) {
      const auto &term = atom.terms[column];
      if (!term.use_in_lookup)
        continue;
      if (!term.is_variable) {
        key.push(term.constant);
      } else {
        if (!binding[term.variable])
          throw std::logic_error(
              "planned Datalog lookup uses an unbound variable");
        key.push(*binding[term.variable]);
      }
    }
    return key;
  }

  std::vector<std::any> evaluateAggregate(const AggregateIR &aggregate,
                                          const AtomPlan &source,
                                          RelationStorage &source_relation,
                                          const KeyView &key, Binding &binding,
                                          Scheduler *aggregate_scheduler,
                                          ExecutionStats &evaluation_stats) {
    auto evaluate_serial = [&] {
      AggregateForEach for_each = [&](const AggregateConsumer &consumer) {
        source_relation.forEachMatching(
            key, evaluation_stats, [&](const Row &row) {
              matchRow(source, row, binding, [&] {
                std::any value = aggregate.projection.evaluate(binding);
                consumer(value);
              });
            });
      };
      return aggregate.evaluate(for_each);
    };
    if (!aggregate.reducer ||
        !aggregate.reducer->properties.canRunInParallel() ||
        !aggregate_scheduler || aggregate_scheduler->workerCount() <= 1)
      return evaluate_serial();

    const std::size_t matching_rows =
        source_relation.matchingCandidateCount(key, evaluation_stats);
    if (matching_rows == 0) {
      std::any state = aggregate.reducer->make_state();
      return aggregate.reducer->finish(state);
    }
    evaluation_stats.tuples_scanned += matching_rows;

    const std::size_t grain =
        std::max<std::size_t>(1, options().parallel_grain_size);
    const std::size_t available_chunks = (matching_rows + grain - 1) / grain;
    const std::size_t chunk_count =
        std::min(aggregate_scheduler->workerCount(), available_chunks);
    if (chunk_count <= 1)
      return evaluate_serial();

    std::vector<ReducerIR> reducers(chunk_count, *aggregate.reducer);
    std::vector<std::any> states;
    states.reserve(chunk_count);
    for (const ReducerIR &reducer : reducers)
      states.push_back(reducer.make_state());

    aggregate_scheduler->parallelFor(chunk_count, [&](std::size_t chunk) {
      Binding chunk_binding = binding;
      const std::size_t begin = matching_rows * chunk / chunk_count;
      const std::size_t end = matching_rows * (chunk + 1) / chunk_count;
      source_relation.forEachMatchingSlice(
          key, begin, end, [&](const Row &row) {
            matchRow(source, row, chunk_binding, [&] {
              reducers[chunk].add(states[chunk],
                                  aggregate.projection.evaluate(chunk_binding));
            });
          });
    });
    evaluation_stats.parallel_tasks += chunk_count;
    evaluation_stats.parallel_aggregate_tasks += chunk_count;

    for (std::size_t chunk = 1; chunk < chunk_count; ++chunk)
      reducers[0].merge(states[0], states[chunk]);
    return reducers[0].finish(states[0]);
  }

  bool isCurrentDeltaRow(RelationId relation_id, const Row &row,
                         const DeltaMap &delta,
                         const DeltaHashMap &delta_hashes) {
    auto rows = delta.find(relation_id);
    auto hashes = delta_hashes.find(relation_id);
    if (rows == delta.end() || hashes == delta_hashes.end())
      return false;
    auto bucket = hashes->second.find(relation(relation_id).candidateHash(row));
    if (bucket == hashes->second.end())
      return false;
    for (std::size_t row_index : bucket->second) {
      const Row &delta_row = rows->second[row_index];
      if (relation(relation_id).rowsEqual(row, delta_row))
        return true;
    }
    return false;
  }

  void evaluateRule(
      std::size_t rule_index, Binding &binding,
      std::optional<std::size_t> delta_item, std::optional<DeltaView> delta,
      CandidateMap &candidates, ExecutionStats &evaluation_stats,
      Scheduler *aggregate_scheduler = nullptr,
      const std::unordered_set<RelationId> *recursive_relations = nullptr,
      const DeltaMap *current_delta = nullptr,
      const DeltaHashMap *delta_hashes = nullptr) {
    const RulePlan &rule = plan.rules[rule_index];
    ++evaluation_stats.rule_evaluations;
    traceRule(rule_index, delta_item);
    std::function<void(std::size_t)> evaluate_item =
        [&](std::size_t item_index) {
          if (item_index == rule.body.size()) {
            Row row;
            row.reserve(rule.head.size());
            for (const HeadTermPlan &term : rule.head) {
              if (term.kind == HeadTermPlan::Kind::Variable)
                row.push_back(*binding[term.variable]);
              else if (term.kind == HeadTermPlan::Kind::Constant)
                row.push_back(term.constant);
              else
                row.push_back(term.expression.evaluate(binding));
            }
            candidates[rule.head_relation].push_back(std::move(row));
            return;
          }

          const PhysicalOp &item = rule.body[item_index];
          if (item.code == OpCode::Filter) {
            if (std::any_cast<bool>(item.filter.evaluate(binding)))
              evaluate_item(item_index + 1);
            return;
          }

          if (item.code == OpCode::AntiLookup) {
            const AtomPlan &atom = item.atom;
            bool found = false;
            auto test_row = [&](const Row &row) {
              if (found)
                return;
              matchRow(atom, row, binding, [&] { found = true; });
            };
            const KeyView key = lookupKey(atom, binding);
            relation(atom.relation)
                .forEachMatching(key, evaluation_stats, test_row);
            if (!found)
              evaluate_item(item_index + 1);
            return;
          }

          if (item.code == OpCode::Aggregate) {
            const AggregateIR &aggregate = item.aggregate;
            const AtomPlan &source = item.atom;
            const KeyView key = lookupKey(source, binding);
            std::vector<std::any> results = evaluateAggregate(
                aggregate, source, relation(source.relation), key, binding,
                aggregate_scheduler, evaluation_stats);
            for (std::any &result : results) {
              binding[aggregate.output_var] = std::move(result);
              evaluate_item(item_index + 1);
              binding[aggregate.output_var].reset();
            }
            return;
          }

          const AtomPlan &atom = item.atom;
          auto continue_with = [&](const Row &row) {
            matchRow(atom, row, binding,
                     [&] { evaluate_item(item_index + 1); });
          };

          if (delta_item && *delta_item == item_index) {
            for (std::size_t row_index = delta->begin; row_index < delta->end;
                 ++row_index) {
              ++evaluation_stats.tuples_scanned;
              continue_with((*delta->rows)[row_index]);
            }
            return;
          }

          const KeyView key = lookupKey(atom, binding);
          auto continue_with_old_total = [&](const Row &row) {
            if (delta_item && item_index < *delta_item && recursive_relations &&
                current_delta && delta_hashes &&
                recursive_relations->count(atom.relation) != 0 &&
                isCurrentDeltaRow(atom.relation, row, *current_delta,
                                  *delta_hashes))
              return;
            continue_with(row);
          };
          relation(atom.relation)
              .forEachMatching(key, evaluation_stats, continue_with_old_total);
        };

    evaluate_item(0);
  }

  void coalesceLocalLattices(CandidateMap &candidates) {
    for (auto &[relation_id, rows] : candidates) {
      RelationStorage &storage = relation(relation_id);
      if (storage.definition().kind == RelationKind::Lattice)
        rows = storage.coalesce(std::move(rows));
    }
  }

  std::vector<Row> coalesceCandidates(RelationStorage &storage,
                                      std::vector<Row> rows,
                                      Scheduler &scheduler) {
    const std::size_t grain =
        std::max<std::size_t>(1, options().parallel_grain_size);
    const std::size_t shard_count =
        std::min(scheduler.workerCount(), (rows.size() + grain - 1) / grain);
    if (shard_count <= 1)
      return storage.coalesce(std::move(rows));

    std::vector<std::vector<Row>> shards(shard_count);
    for (Row &row : rows) {
      const std::size_t shard = storage.candidateHash(row) % shard_count;
      shards[shard].push_back(std::move(row));
    }
    scheduler.parallelFor(shard_count, [&](std::size_t shard) {
      shards[shard] = storage.coalesce(std::move(shards[shard]));
    });
    stats.parallel_tasks += shard_count;
    stats.parallel_merge_tasks += shard_count;

    std::vector<Row> result;
    for (std::vector<Row> &shard : shards) {
      result.insert(result.end(), std::make_move_iterator(shard.begin()),
                    std::make_move_iterator(shard.end()));
    }
    return result;
  }

  DeltaMap mergeCandidates(CandidateMap candidates, Scheduler &scheduler) {
    DeltaMap inserted;
    for (auto &[relation_id, rows] : candidates) {
      RelationStorage &storage = relation(relation_id);
      std::vector<Row> coalesced =
          coalesceCandidates(storage, std::move(rows), scheduler);
      RelationStorage::BatchMergeResult merged = storage.mergeDerivedCoalesced(
          std::move(coalesced), scheduler, options().parallel_grain_size);
      stats.parallel_tasks += merged.parallel_tasks;
      stats.parallel_merge_tasks += merged.parallel_tasks;
      stats.inserted_facts += merged.changed.size();
      if (!merged.changed.empty())
        inserted.emplace(relation_id, std::move(merged.changed));
    }
    return inserted;
  }

  static bool hasRows(const DeltaMap &delta) {
    for (const auto &[relation, rows] : delta) {
      (void)relation;
      if (!rows.empty())
        return true;
    }
    return false;
  }

  static std::size_t deltaSize(const DeltaMap &delta) {
    std::size_t size = 0;
    for (const auto &[relation, rows] : delta) {
      (void)relation;
      size += rows.size();
    }
    return size;
  }

  void runNonRecursive(const PlannedSCC &scc, Binding &binding,
                       Scheduler &scheduler) {
    std::vector<EvaluationTask> tasks;
    for (std::size_t rule_index : scc.rules) {
      const RulePlan &rule = plan.rules[rule_index];
      std::optional<std::size_t> driver_item;
      for (std::size_t item_index = 0; item_index < rule.body.size();
           ++item_index) {
        if (rule.body[item_index].code == OpCode::Scan) {
          driver_item = item_index;
          break;
        }
      }

      if (!driver_item) {
        tasks.push_back({rule_index, std::nullopt, std::nullopt});
        continue;
      }

      const AtomPlan &driver = rule.body[*driver_item].atom;
      const std::vector<Row> &rows = relation(driver.relation).rows();
      if (rows.empty())
        continue;
      const std::size_t grain =
          scheduler.workerCount() > 1
              ? std::max<std::size_t>(1, options().parallel_grain_size)
              : rows.size();
      for (std::size_t begin = 0; begin < rows.size(); begin += grain) {
        tasks.push_back(
            {rule_index, driver_item,
             DeltaView{&rows, begin, std::min(rows.size(), begin + grain)}});
      }
    }

    if (tasks.empty())
      return;

    if (tasks.size() == 1) {
      CandidateMap candidates;
      const EvaluationTask &task = tasks.front();
      evaluateRule(task.rule_index, binding, task.delta_item, task.delta,
                   candidates, stats, &scheduler);
      mergeCandidates(std::move(candidates), scheduler);
      return;
    }

    std::vector<CandidateMap> task_candidates(tasks.size());
    std::vector<ExecutionStats> task_stats(tasks.size());
    const bool parallel_batch = scheduler.workerCount() > 1;
    const bool coalesce_locally = tasks.size() >= scheduler.workerCount();
    scheduler.parallelFor(tasks.size(), [&](std::size_t task_index) {
      Binding task_binding(plan.variable_count);
      const EvaluationTask &task = tasks[task_index];
      evaluateRule(task.rule_index, task_binding, task.delta_item, task.delta,
                   task_candidates[task_index], task_stats[task_index]);
      if (coalesce_locally)
        coalesceLocalLattices(task_candidates[task_index]);
      if (parallel_batch)
        task_stats[task_index].parallel_tasks =
            task_stats[task_index].parallel_rule_tasks = 1;
    });

    CandidateMap candidates;
    for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
      mergeStats(stats, task_stats[task_index]);
      for (auto &[relation_id, rows] : task_candidates[task_index]) {
        auto &destination = candidates[relation_id];
        destination.insert(destination.end(),
                           std::make_move_iterator(rows.begin()),
                           std::make_move_iterator(rows.end()));
      }
    }
    mergeCandidates(std::move(candidates), scheduler);
  }

  void runRecursive(const PlannedSCC &scc, Binding &binding,
                    Scheduler &scheduler) {
    DeltaMap current_delta;
    for (RelationId relation_id : scc.relations)
      current_delta[relation_id] = relation(relation_id).rows();

    CandidateMap base_candidates;
    for (std::size_t rule_index : scc.rules) {
      const RulePlan &rule = plan.rules[rule_index];
      bool has_recursive_atom = false;
      for (const PhysicalOp &item : rule.body) {
        if (item.code == OpCode::Scan)
          has_recursive_atom = has_recursive_atom ||
                               scc.relation_set.count(item.atom.relation) != 0;
      }
      if (!has_recursive_atom)
        evaluateRule(rule_index, binding, std::nullopt, std::nullopt,
                     base_candidates, stats, &scheduler);
    }
    DeltaMap base_delta =
        mergeCandidates(std::move(base_candidates), scheduler);
    for (auto &[relation_id, rows] : base_delta) {
      auto &destination = current_delta[relation_id];
      destination.insert(destination.end(),
                         std::make_move_iterator(rows.begin()),
                         std::make_move_iterator(rows.end()));
    }

    std::size_t iteration = 0;
    while (hasRows(current_delta)) {
      stats.peak_delta = std::max(stats.peak_delta, deltaSize(current_delta));
      traceDelta(iteration, current_delta);
      ++stats.fixpoint_iterations;
      ++iteration;
      DeltaHashMap delta_hashes;
      for (const auto &[relation_id, rows] : current_delta) {
        auto &hashes = delta_hashes[relation_id];
        hashes.reserve(rows.size());
        for (std::size_t row_index = 0; row_index < rows.size(); ++row_index)
          hashes[relation(relation_id).candidateHash(rows[row_index])]
              .push_back(row_index);
      }
      std::vector<EvaluationTask> tasks;
      for (std::size_t rule_index : scc.rules) {
        const RulePlan &rule = plan.rules[rule_index];
        for (std::size_t item_index = 0; item_index < rule.body.size();
             ++item_index) {
          const PhysicalOp &item = rule.body[item_index];
          if (item.code != OpCode::Scan ||
              !scc.relation_set.count(item.atom.relation))
            continue;
          auto delta_it = current_delta.find(item.atom.relation);
          if (delta_it == current_delta.end() || delta_it->second.empty())
            continue;
          const std::size_t grain =
              scheduler.workerCount() > 1
                  ? std::max<std::size_t>(1, options().parallel_grain_size)
                  : delta_it->second.size();
          for (std::size_t begin = 0; begin < delta_it->second.size();
               begin += grain) {
            tasks.push_back(
                {rule_index, item_index,
                 DeltaView{&delta_it->second, begin,
                           std::min(delta_it->second.size(), begin + grain)}});
          }
        }
      }

      std::vector<CandidateMap> task_candidates(tasks.size());
      std::vector<ExecutionStats> task_stats(tasks.size());
      const bool parallel_batch =
          scheduler.workerCount() > 1 && tasks.size() > 1;
      const bool coalesce_locally = tasks.size() >= scheduler.workerCount();
      scheduler.parallelFor(tasks.size(), [&](std::size_t task_index) {
        Binding task_binding(plan.variable_count);
        const EvaluationTask &task = tasks[task_index];
        evaluateRule(task.rule_index, task_binding, task.delta_item, task.delta,
                     task_candidates[task_index], task_stats[task_index],
                     nullptr, &scc.relation_set, &current_delta, &delta_hashes);
        if (coalesce_locally)
          coalesceLocalLattices(task_candidates[task_index]);
        if (parallel_batch)
          task_stats[task_index].parallel_tasks =
              task_stats[task_index].parallel_rule_tasks = 1;
      });

      CandidateMap next_candidates;
      for (std::size_t task_index = 0; task_index < tasks.size();
           ++task_index) {
        mergeStats(stats, task_stats[task_index]);
        for (auto &[relation_id, rows] : task_candidates[task_index]) {
          auto &destination = next_candidates[relation_id];
          destination.insert(destination.end(),
                             std::make_move_iterator(rows.begin()),
                             std::make_move_iterator(rows.end()));
        }
      }
      current_delta = mergeCandidates(std::move(next_candidates), scheduler);
    }
    traceDelta(iteration, current_delta);
  }

  void run(const ExecutionOptions &options) {
    ExecutionContext execution_context{options};
    ExecutionScope execution_scope(*this, execution_context);
    stats = {};
    stats.planned_reorders = plan.planned_reorders;
    stats.scc_count = plan.sccs.size();
    stats.relation_count = context->relations.size();

    std::vector<bool> dirty_scc(plan.sccs.size(), false);
    auto mark_dirty = [&](std::size_t root) {
      std::vector<std::size_t> worklist{root};
      while (!worklist.empty()) {
        const std::size_t scc_index = worklist.back();
        worklist.pop_back();
        if (dirty_scc[scc_index])
          continue;
        dirty_scc[scc_index] = true;
        for (std::size_t dependent : plan.scc_dependents[scc_index])
          worklist.push_back(dependent);
      }
    };
    if (!has_completed_run) {
      std::fill(dirty_scc.begin(), dirty_scc.end(), true);
    } else {
      for (RelationId relation_id = 0; relation_id < context->relations.size();
           ++relation_id) {
        if (relation(relation_id).baseVersion() ==
            completed_base_versions[relation_id])
          continue;
        mark_dirty(plan.relation_scc[relation_id]);
      }
    }

    if (std::none_of(dirty_scc.begin(), dirty_scc.end(),
                     [](bool dirty) { return dirty; })) {
      collectStorageStats();
      return;
    }

    // Facts inserted through Relation::insert are base facts.  Rebuild every
    // affected SCC from that base layer, retaining already-stable lower strata.
    // This is additive rerun support with correct retractions for negation and
    // aggregates, rather than pretending that all programs are monotone.
    for (std::size_t scc_index = 0; scc_index < plan.sccs.size(); ++scc_index) {
      if (!dirty_scc[scc_index])
        continue;
      for (RelationId relation_id : plan.sccs[scc_index].relations)
        relation(relation_id).discardDerived();
    }

    Binding binding(plan.variable_count);
    SerialScheduler serial_scheduler;
    std::unique_ptr<ThreadScheduler> thread_scheduler;
    Scheduler *scheduler = options.scheduler;
    if (!scheduler && options.worker_count > 1) {
      thread_scheduler =
          std::make_unique<ThreadScheduler>(options.worker_count);
      scheduler = thread_scheduler.get();
    }
    if (!scheduler)
      scheduler = &serial_scheduler;
    for (std::size_t scc_index = 0; scc_index < plan.sccs.size(); ++scc_index) {
      if (!dirty_scc[scc_index])
        continue;
      const PlannedSCC &scc = plan.sccs[scc_index];
      if (options.trace_scc) {
        std::lock_guard<std::mutex> lock(trace_mutex);
        traceStream() << "SCC " << scc_index << " stratum=" << scc.stratum
                      << " recursive=" << (scc.recursive ? "yes" : "no")
                      << " relations=";
        for (RelationId relation_id : scc.relations)
          traceStream() << relation(relation_id).definition().name << ' ';
        traceStream() << '\n';
      }
      if (scc.recursive)
        runRecursive(scc, binding, *scheduler);
      else
        runNonRecursive(scc, binding, *scheduler);
    }
    completed_base_versions.clear();
    completed_base_versions.reserve(context->relations.size());
    for (const auto &storage : context->relations) {
      completed_base_versions.push_back(storage->baseVersion());
    }
    collectStorageStats();
    has_completed_run = true;
  }
};

CompiledProgram Program::compile() const {
  ExecutionPlan plan;
  std::vector<RuleIR> semantic_rules =
      planAndValidateRules(rules_, context_->impl_->relations,
                           context_->impl_->variables, plan.planned_reorders);
  plan.variable_count = context_->impl_->variables.size();
  const std::vector<DependencyEdge> dependencies =
      collectDependencies(semantic_rules);
  const std::vector<std::size_t> strata =
      computeStrata(dependencies, context_->impl_->relations.size());
  plan.sccs = buildSCCPlan(semantic_rules, dependencies, strata,
                           context_->impl_->relations.size());
  plan.rules.reserve(semantic_rules.size());
  for (const RuleIR &rule : semantic_rules)
    plan.rules.push_back(internal::lowerRulePlan(rule));
  for (const RulePlan &rule : plan.rules) {
    for (const PhysicalOp &operation : rule.body) {
      if (operation.code == OpCode::Scan ||
          operation.code == OpCode::AntiLookup ||
          operation.code == OpCode::Aggregate) {
        context_->impl_->relations.at(operation.atom.relation)
            ->ensureIndex(operation.atom.lookup_mask);
      }
    }
  }
  auto impl = std::make_unique<CompiledProgram::Impl>();
  impl->context = context_->impl_;
  impl->plan = std::move(plan);
  impl->plan.relation_scc.assign(context_->impl_->relations.size(), 0);
  for (std::size_t scc_index = 0; scc_index < impl->plan.sccs.size();
       ++scc_index) {
    for (RelationId relation_id : impl->plan.sccs[scc_index].relations)
      impl->plan.relation_scc[relation_id] = scc_index;
  }
  impl->plan.scc_dependents.resize(impl->plan.sccs.size());
  for (const DependencyEdge &dependency : dependencies) {
    const std::size_t source_scc = impl->plan.relation_scc[dependency.source];
    const std::size_t target_scc = impl->plan.relation_scc[dependency.target];
    if (source_scc != target_scc)
      impl->plan.scc_dependents[source_scc].push_back(target_scc);
  }
  for (auto &dependents : impl->plan.scc_dependents) {
    std::sort(dependents.begin(), dependents.end());
    dependents.erase(std::unique(dependents.begin(), dependents.end()),
                     dependents.end());
  }
  return CompiledProgram(std::move(impl));
}

CompiledProgram::CompiledProgram(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CompiledProgram::CompiledProgram(CompiledProgram &&) noexcept = default;
CompiledProgram &
CompiledProgram::operator=(CompiledProgram &&) noexcept = default;
CompiledProgram::~CompiledProgram() = default;

void CompiledProgram::run() { impl_->run(ExecutionOptions{}); }

void CompiledProgram::run(const ExecutionOptions &options) {
  impl_->run(options);
}

const ExecutionStats &CompiledProgram::stats() const { return impl_->stats; }

} // namespace lotus::datalog
