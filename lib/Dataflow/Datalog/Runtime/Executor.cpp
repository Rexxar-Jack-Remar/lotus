#include "Dataflow/Datalog/ContextInternal.h"
#include "Dataflow/Datalog/Internal.h"
#include "Dataflow/Datalog/Program.h"

#include <algorithm>
#include <any>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::datalog {

using internal::buildSCCPlan;
using internal::collectDependencies;
using internal::computeStrata;
using internal::DependencyEdge;
using internal::ExecutionPlan;
using internal::planAndValidateRules;
using internal::PlannedSCC;
using internal::RelationStorage;
using internal::Row;

struct CompiledProgram::Impl {
  Context *context = nullptr;
  ExecutionPlan plan;
  ExecutionStats stats;
  const ExecutionOptions *active_options = nullptr;
  std::mutex trace_mutex;

  RelationStorage &relation(RelationId id) {
    return *context->impl_->relations.at(id);
  }

  using CandidateMap = std::unordered_map<RelationId, std::vector<Row>>;
  using DeltaMap = std::unordered_map<RelationId, std::vector<Row>>;

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

  std::ostream &traceStream() {
    if (active_options && active_options->trace_stream)
      return *active_options->trace_stream;
    return std::cerr;
  }

  void traceRule(std::size_t rule_index,
                 std::optional<std::size_t> delta_item) {
    if (!active_options || !active_options->trace_rule)
      return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    const RuleIR &rule = plan.rules[rule_index];
    traceStream() << "rule " << rule_index << " -> "
                  << relation(rule.head.relation).definition().name;
    if (delta_item)
      traceStream() << " delta-body=" << *delta_item;
    traceStream() << '\n';
  }

  void traceDelta(std::size_t iteration, const DeltaMap &delta) {
    if (!active_options || !active_options->trace_delta)
      return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    traceStream() << "iteration " << iteration << ':';
    for (const auto &[relation_id, rows] : delta) {
      traceStream() << ' ' << relation(relation_id).definition().name << '='
                    << rows.size();
    }
    traceStream() << '\n';
  }

  bool matchRow(const AtomIR &atom, const Row &row, Binding &binding,
                const std::function<void()> &continuation) {
    std::vector<VarId> newly_bound;
    for (std::size_t column = 0; column < atom.args.size(); ++column) {
      const TermIR &term = atom.args[column];
      const ColumnType &type =
          relation(atom.relation).definition().columns[column];
      if (term.kind == TermIR::Kind::Constant) {
        if (!type.equal(term.constant, row[column])) {
          for (VarId variable : newly_bound)
            binding[variable].reset();
          return false;
        }
        continue;
      }

      if (term.kind != TermIR::Kind::Variable)
        throw std::logic_error("expression term reached body matcher");
      if (binding[term.variable]) {
        if (!type.equal(*binding[term.variable], row[column])) {
          for (VarId variable : newly_bound)
            binding[variable].reset();
          return false;
        }
      } else {
        binding[term.variable].bindReference(row[column]);
        newly_bound.push_back(term.variable);
      }
    }

    continuation();
    for (VarId variable : newly_bound)
      binding[variable].reset();
    return true;
  }

  std::pair<ColumnMask, Row> lookupKey(const AtomIR &atom,
                                       const Binding &binding) {
    ColumnMask mask = 0;
    Row key;
    for (std::size_t column = 0; column < atom.args.size(); ++column) {
      const TermIR &term = atom.args[column];
      if (term.kind == TermIR::Kind::Constant) {
        mask |= ColumnMask{1} << column;
        key.push_back(term.constant);
      } else if (term.kind == TermIR::Kind::Variable &&
                 binding[term.variable]) {
        mask |= ColumnMask{1} << column;
        key.push_back(*binding[term.variable]);
      }
    }
    return {mask, std::move(key)};
  }

  std::vector<std::any>
  evaluateAggregate(const AggregateIR &aggregate,
                    const std::vector<const Row *> &matching_rows,
                    Binding &binding, Scheduler *aggregate_scheduler,
                    ExecutionStats &evaluation_stats) {
    auto evaluate_serial = [&] {
      AggregateForEach for_each = [&](const AggregateConsumer &consumer) {
        for (const Row *row : matching_rows) {
          matchRow(aggregate.source, *row, binding, [&] {
            std::any value = aggregate.projection.evaluate(binding);
            consumer(value);
          });
        }
      };
      return aggregate.evaluate(for_each);
    };
    if (!aggregate.reducer || !aggregate_scheduler ||
        aggregate_scheduler->workerCount() <= 1 || matching_rows.empty())
      return evaluate_serial();

    const std::size_t grain =
        std::max<std::size_t>(1, active_options->parallel_grain_size);
    const std::size_t available_chunks =
        (matching_rows.size() + grain - 1) / grain;
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
      const std::size_t begin = matching_rows.size() * chunk / chunk_count;
      const std::size_t end = matching_rows.size() * (chunk + 1) / chunk_count;
      for (std::size_t index = begin; index < end; ++index) {
        matchRow(aggregate.source, *matching_rows[index], chunk_binding, [&] {
          reducers[chunk].add(states[chunk],
                              aggregate.projection.evaluate(chunk_binding));
        });
      }
    });
    evaluation_stats.parallel_tasks += chunk_count;
    evaluation_stats.parallel_aggregate_tasks += chunk_count;

    for (std::size_t chunk = 1; chunk < chunk_count; ++chunk)
      reducers[0].merge(states[0], states[chunk]);
    return reducers[0].finish(states[0]);
  }

  void evaluateRule(std::size_t rule_index, Binding &binding,
                    std::optional<std::size_t> delta_item,
                    std::optional<DeltaView> delta, CandidateMap &candidates,
                    ExecutionStats &evaluation_stats,
                    Scheduler *aggregate_scheduler = nullptr) {
    const RuleIR &rule = plan.rules[rule_index];
    ++evaluation_stats.rule_evaluations;
    traceRule(rule_index, delta_item);
    std::function<void(std::size_t)> evaluate_item = [&](std::size_t
                                                             item_index) {
      if (item_index == rule.body.size()) {
        Row row;
        row.reserve(rule.head.args.size());
        for (const TermIR &term : rule.head.args) {
          if (term.kind == TermIR::Kind::Variable)
            row.push_back(*binding[term.variable]);
          else if (term.kind == TermIR::Kind::Constant)
            row.push_back(term.constant);
          else
            row.push_back(term.expression.evaluate(binding));
        }
        candidates[rule.head.relation].push_back(std::move(row));
        return;
      }

      const BodyItemIR &item = rule.body[item_index];
      if (const auto *filter = std::get_if<FilterIR>(&item)) {
        if (std::any_cast<bool>(filter->predicate.evaluate(binding)))
          evaluate_item(item_index + 1);
        return;
      }

      if (const auto *negation = std::get_if<NegAtomIR>(&item)) {
        const AtomIR &atom = negation->atom;
        bool found = false;
        auto test_row = [&](const Row &row) {
          if (found)
            return;
          matchRow(atom, row, binding, [&] { found = true; });
        };
        auto [mask, key] = lookupKey(atom, binding);
        relation(atom.relation)
            .forEachMatching(mask, key, evaluation_stats, test_row);
        if (!found)
          evaluate_item(item_index + 1);
        return;
      }

      if (const auto *aggregate = std::get_if<AggregateIR>(&item)) {
        const AtomIR &source = aggregate->source;
        auto [mask, key] = lookupKey(source, binding);
        std::vector<const Row *> matching_rows =
            relation(source.relation).matchingRows(mask, key, evaluation_stats);
        std::vector<std::any> results =
            evaluateAggregate(*aggregate, matching_rows, binding,
                              aggregate_scheduler, evaluation_stats);
        for (std::any &result : results) {
          binding[aggregate->output_var] = std::move(result);
          evaluate_item(item_index + 1);
          binding[aggregate->output_var].reset();
        }
        return;
      }

      const AtomIR &atom = std::get<AtomIR>(item);
      auto continue_with = [&](const Row &row) {
        matchRow(atom, row, binding, [&] { evaluate_item(item_index + 1); });
      };

      if (delta_item && *delta_item == item_index) {
        for (std::size_t row_index = delta->begin; row_index < delta->end;
             ++row_index) {
          ++evaluation_stats.tuples_scanned;
          continue_with((*delta->rows)[row_index]);
        }
        return;
      }

      auto [mask, key] = lookupKey(atom, binding);
      relation(atom.relation)
          .forEachMatching(mask, key, evaluation_stats, continue_with);
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
        std::max<std::size_t>(1, active_options->parallel_grain_size);
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
      RelationStorage::BatchMergeResult merged = storage.mergeCoalesced(
          std::move(coalesced), scheduler, active_options->parallel_grain_size);
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
      const RuleIR &rule = plan.rules[rule_index];
      std::optional<std::size_t> driver_item;
      for (std::size_t item_index = 0; item_index < rule.body.size();
           ++item_index) {
        if (std::holds_alternative<AtomIR>(rule.body[item_index])) {
          driver_item = item_index;
          break;
        }
      }

      if (!driver_item) {
        tasks.push_back({rule_index, std::nullopt, std::nullopt});
        continue;
      }

      const AtomIR &driver = std::get<AtomIR>(rule.body[*driver_item]);
      const std::vector<Row> &rows = relation(driver.relation).rows();
      if (rows.empty())
        continue;
      const std::size_t grain =
          scheduler.workerCount() > 1
              ? std::max<std::size_t>(1, active_options->parallel_grain_size)
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
      const RuleIR &rule = plan.rules[rule_index];
      bool has_recursive_atom = false;
      for (const BodyItemIR &item : rule.body) {
        if (const auto *atom = std::get_if<AtomIR>(&item))
          has_recursive_atom =
              has_recursive_atom || scc.relation_set.count(atom->relation) != 0;
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
      std::vector<EvaluationTask> tasks;
      for (std::size_t rule_index : scc.rules) {
        const RuleIR &rule = plan.rules[rule_index];
        for (std::size_t item_index = 0; item_index < rule.body.size();
             ++item_index) {
          const auto *atom = std::get_if<AtomIR>(&rule.body[item_index]);
          if (!atom || !scc.relation_set.count(atom->relation))
            continue;
          auto delta_it = current_delta.find(atom->relation);
          if (delta_it == current_delta.end() || delta_it->second.empty())
            continue;
          const std::size_t grain =
              scheduler.workerCount() > 1
                  ? std::max<std::size_t>(1,
                                          active_options->parallel_grain_size)
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
                     task_candidates[task_index], task_stats[task_index]);
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
    active_options = &options;
    stats = {};
    stats.planned_reorders = plan.planned_reorders;
    stats.scc_count = plan.sccs.size();
    stats.relation_count = context->impl_->relations.size();
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
    for (const auto &storage : context->impl_->relations) {
      stats.total_facts += storage->rows().size();
      stats.index_count += storage->indexCount();
      stats.index_entries += storage->indexEntries();
      stats.index_memory_bytes += storage->indexMemoryBytes();
    }
    active_options = nullptr;
  }
};

CompiledProgram Program::compile() const {
  ExecutionPlan plan;
  plan.rules =
      planAndValidateRules(rules_, context_->impl_->relations,
                           context_->impl_->variables, plan.planned_reorders);
  plan.variable_count = context_->impl_->variables.size();
  const std::vector<DependencyEdge> dependencies =
      collectDependencies(plan.rules);
  const std::vector<std::size_t> strata =
      computeStrata(dependencies, context_->impl_->relations.size());
  plan.sccs = buildSCCPlan(plan.rules, dependencies, strata,
                           context_->impl_->relations.size());
  auto impl = std::make_unique<CompiledProgram::Impl>();
  impl->context = context_;
  impl->plan = std::move(plan);
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
