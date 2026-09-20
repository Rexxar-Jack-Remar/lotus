#include "Alias/InclusionBased/GPG/Analysis.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <iterator>
#include <utility>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace lotus::gpg {

namespace {

bool isDefinedFunction(const llvm::Function *function) {
  return function && !function->isDeclaration() && !function->isIntrinsic();
}

AccessSet gpuSources(const GPUSet &gpus) {
  AccessSet result;
  for (const GPU &gpu : gpus)
    result.insert(gpu.source);
  return result;
}

} // namespace

GPGAnalysisEngine::GPGAnalysisEngine(llvm::Module &module, GPGConfig config)
    : module_(module), config_(std::move(config)), model_(module),
      frontend_(model_, config_) {}

const GPG *GPGAnalysisEngine::summary(const llvm::Function *function) const {
  auto found = summaries_.find(function);
  return found == summaries_.end() ? nullptr : &found->second;
}

const GPG *GPGAnalysisEngine::initialGPG(const llvm::Function *function) const {
  auto found = initial_graphs_.find(function);
  return found == initial_graphs_.end() ? nullptr : &found->second;
}

void GPGAnalysisEngine::buildInitialGraphs() {
  initial_graphs_.clear();
  summaries_.clear();
  stats_ = {};

  for (llvm::Function &function : module_) {
    if (function.isIntrinsic())
      continue;
    if (function.isDeclaration()) {
      GPG external;
      GPB start{1, GPBKind::Start};
      GPB effect{2, GPBKind::Normal};
      GPB end{3, GPBKind::End};

      if (function.getReturnType()->isPointerTy()) {
        GPU result;
        result.source = model_.access(model_.returnLocation(&function),
                                      IndirectionList::dereferences(1));
        result.target = model_.access(model_.unknownLocation(), {});
        result.source_may_alias_multiple = true;
        effect.gpus.insert(std::move(result));
      }
      for (llvm::Argument &argument : function.args()) {
        if (!argument.getType()->isPointerTy())
          continue;
        GPU side_effect;
        Access formal = model_.access(model_.valueLocation(&argument),
                                      IndirectionList::dereferences(1));
        side_effect.source = formal;
        side_effect.source.indirections =
            formal.indirections.append(IndirectionList::dereferences(1));
        side_effect.source.type = model_.typeAt(
            side_effect.source.location, side_effect.source.indirections);
        side_effect.target = model_.access(model_.unknownLocation(), {});
        side_effect.source_may_alias_multiple = true;
        effect.gpus.insert(std::move(side_effect));
      }
      effect.original_gpus = effect.gpus;
      external.addBlock(std::move(start));
      external.addBlock(std::move(effect));
      external.addBlock(std::move(end));
      external.setEntry(1);
      external.setExit(3);
      external.addEdge(1, 2);
      external.addEdge(2, 3);
      summaries_[&function] = std::move(external);
      continue;
    }

    GPG graph = frontend_.buildInitialGPG(function);
    if (function.getName() == "main") {
      GPUSet boundary = graph.boundaryDefinitions();
      for (const GPU &initializer : frontend_.globalInitializers()) {
        for (auto iterator = boundary.begin(); iterator != boundary.end();) {
          if (iterator->source.sameSource(initializer.source))
            iterator = boundary.erase(iterator);
          else
            ++iterator;
        }
        boundary.insert(initializer);
      }
      graph.setBoundaryDefinitions(std::move(boundary));
    }

    GPGStats graph_stats = graph.stats();
    stats_.initial_gpbs += graph_stats.blocks;
    stats_.initial_gpus += graph_stats.gpus;
    ++stats_.functions;
    initial_graphs_[&function] = std::move(graph);
  }
}

std::map<const llvm::Function *, GPGAnalysisEngine::FunctionSet>
GPGAnalysisEngine::buildCallees() const {
  std::map<const llvm::Function *, FunctionSet> result;
  for (const auto &[function, graph] : initial_graphs_) {
    FunctionSet &callees = result[function];
    for (const auto &[id, block] : graph.blocks()) {
      (void)id;
      callees.insert(block.callees.begin(), block.callees.end());
      if (block.kind != GPBKind::IndirectCall || !block.callsite)
        continue;
      auto targets = indirect_targets_.find(block.callsite);
      if (targets != indirect_targets_.end())
        callees.insert(targets->second.begin(), targets->second.end());
    }
  }
  return result;
}

std::map<const llvm::Function *, GPGAnalysisEngine::FunctionSet>
GPGAnalysisEngine::buildCallers(
    const std::map<const llvm::Function *, FunctionSet> &callees) const {
  std::map<const llvm::Function *, FunctionSet> result;
  for (const auto &[caller, targets] : callees) {
    result.try_emplace(caller);
    for (const llvm::Function *callee : targets) {
      if (isDefinedFunction(callee))
        result[callee].insert(caller);
    }
  }
  return result;
}

std::vector<GPGAnalysisEngine::SCC> GPGAnalysisEngine::computeSCCs(
    const std::map<const llvm::Function *, FunctionSet> &callees) const {
  std::map<const llvm::Function *, unsigned> index;
  std::map<const llvm::Function *, unsigned> lowlink;
  FunctionSet on_stack;
  std::vector<const llvm::Function *> stack;
  std::vector<SCC> result;
  unsigned next_index = 0;

  std::function<void(const llvm::Function *)> connect =
      [&](const llvm::Function *function) {
        index[function] = next_index;
        lowlink[function] = next_index++;
        stack.push_back(function);
        on_stack.insert(function);

        auto found = callees.find(function);
        if (found != callees.end()) {
          for (const llvm::Function *callee : found->second) {
            if (!isDefinedFunction(callee))
              continue;
            if (index.count(callee) == 0) {
              connect(callee);
              lowlink[function] = std::min(lowlink[function], lowlink[callee]);
            } else if (on_stack.count(callee) != 0) {
              lowlink[function] = std::min(lowlink[function], index[callee]);
            }
          }
        }

        if (lowlink[function] != index[function])
          return;
        SCC component;
        while (!stack.empty()) {
          const llvm::Function *member = stack.back();
          stack.pop_back();
          on_stack.erase(member);
          component.push_back(member);
          if (member == function)
            break;
        }
        result.push_back(std::move(component));
      };

  for (const auto &[function, graph] : initial_graphs_) {
    (void)graph;
    if (index.count(function) == 0)
      connect(function);
  }
  return result;
}

GPG GPGAnalysisEngine::bottomGPG() const {
  GPG graph;
  GPB bottom;
  bottom.id = 1;
  bottom.kind = GPBKind::BottomCall;
  graph.addBlock(std::move(bottom));
  graph.setEntry(1);
  graph.setExit(1);
  return graph;
}

CallExpansion GPGAnalysisEngine::makeExpansion(const llvm::CallBase &call,
                                               const llvm::Function &callee,
                                               const GPG &callee_summary) {
  CallExpansion expansion;
  expansion.callee = &callee_summary;
  StatementId statement = model_.statementId(&call);

  const auto *formal = callee.arg_begin();
  for (unsigned index = 0;
       index < call.arg_size() && formal != callee.arg_end();
       ++index, ++formal) {
    llvm::Value *actual = call.getArgOperand(index);
    if (!formal->getType()->isPointerTy() || !actual->getType()->isPointerTy())
      continue;
    Access source = model_.access(model_.valueLocation(&*formal),
                                  IndirectionList::dereferences(1));
    for (const Access &target : frontend_.readAccesses(actual)) {
      GPU mapping;
      mapping.source = source;
      mapping.target = target;
      mapping.statement = statement;
      mapping.kind = GPUKind::Parameter;
      mapping.origin = &call;
      expansion.parameter_gpus.insert(std::move(mapping));
    }
  }

  if (call.getType()->isPointerTy() && callee.getReturnType()->isPointerTy()) {
    GPU mapping;
    mapping.source = model_.access(model_.valueLocation(&call),
                                   IndirectionList::dereferences(1));
    mapping.target = model_.access(model_.returnLocation(&callee),
                                   IndirectionList::dereferences(1));
    mapping.statement = statement;
    mapping.kind = GPUKind::Return;
    mapping.origin = &call;
    mapping.flow_insensitive = false;
    expansion.return_gpus.insert(std::move(mapping));
  }
  return expansion;
}

GPG GPGAnalysisEngine::instantiate(const llvm::Function &function,
                                   const FunctionSet &current_scc,
                                   bool recursive_refinement) {
  GPG graph = initial_graphs_.at(&function);
  GPG bottom = bottomGPG();
  std::vector<GPBId> calls = graph.callBlocks();
  for (GPBId id : calls) {
    const GPB *block = graph.getBlock(id);
    if (!block || !block->callsite)
      continue;

    TargetSet targets;
    if (block->kind == GPBKind::DirectCall) {
      targets.insert(block->callees.begin(), block->callees.end());
    } else {
      auto found = indirect_targets_.find(block->callsite);
      if (found != indirect_targets_.end())
        targets = found->second;
    }
    if (targets.empty())
      continue;

    std::vector<CallExpansion> alternatives;
    for (const llvm::Function *target : targets) {
      const GPG *callee_summary = nullptr;
      auto summary_it = summaries_.find(target);
      if (summary_it != summaries_.end())
        callee_summary = &summary_it->second;
      if (current_scc.count(target) != 0 && !recursive_refinement)
        callee_summary = nullptr;
      if (!callee_summary)
        callee_summary = &bottom;
      alternatives.push_back(
          makeExpansion(*block->callsite, *target, *callee_summary));
    }
    graph.expandCall(id, alternatives);
  }
  return graph;
}

void GPGAnalysisEngine::optimize(GPG &graph) {
  auto compatible = [&](const llvm::Type *lhs, const llvm::Type *rhs) {
    if (!config_.enable_type_based_non_aliasing)
      return true;
    return model_.typesCompatible(lhs, rhs);
  };

  ReachingPair reaching =
      graph.analyzeReaching(compatible, config_.heap_indirection_limit);
  graph.applyStrengthReduction(reaching, config_.enable_blocking);
  observed_gpus_.insert(graph.supportGPUs().begin(), graph.supportGPUs().end());
  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    observed_gpus_.insert(block.gpus.begin(), block.gpus.end());
  }
  if (config_.enable_dead_gpu_elimination)
    graph.eliminateDeadGPUs(compatible, config_.heap_indirection_limit,
                            config_.enable_blocking, &reaching);
  graph.eliminateEmptyGPBs();
  if (config_.enable_coalescing)
    graph.coalesce(compatible, config_.heap_indirection_limit);
}

GPGAnalysisEngine::SemanticSummary
GPGAnalysisEngine::semantics(const GPG &graph) const {
  auto compatible = [&](const llvm::Type *lhs, const llvm::Type *rhs) {
    if (!config_.enable_type_based_non_aliasing)
      return true;
    return model_.typesCompatible(lhs, rhs);
  };
  ReachingPair reaching =
      graph.analyzeReaching(compatible, config_.heap_indirection_limit);
  return {reaching.without_blocking.out.at(graph.exit()),
          reaching.with_blocking.out.at(graph.exit())};
}

void GPGAnalysisEngine::constructSCC(
    const SCC &scc,
    const std::map<const llvm::Function *, FunctionSet> &callees,
    const std::map<const llvm::Function *, FunctionSet> &callers) {
  FunctionSet members(scc.begin(), scc.end());
  bool recursive = scc.size() > 1;
  if (!recursive && !scc.empty()) {
    auto found = callees.find(scc.front());
    recursive = found != callees.end() && found->second.count(scc.front()) != 0;
  }

  if (!recursive) {
    const llvm::Function *function = scc.front();
    GPG graph = instantiate(*function, members, true);
    optimize(graph);
    summaries_[function] = std::move(graph);
    return;
  }

  std::map<const llvm::Function *, SemanticSummary> previous;
  FunctionSet evaluated;
  for (const llvm::Function *function : scc) {
    if (summaries_.count(function) == 0)
      summaries_[function] = bottomGPG();
    previous[function] = semantics(summaries_[function]);
  }

  std::deque<const llvm::Function *> worklist(scc.begin(), scc.end());
  FunctionSet in_worklist(scc.begin(), scc.end());
  while (!worklist.empty()) {
    const llvm::Function *function = worklist.front();
    worklist.pop_front();
    in_worklist.erase(function);

    GPG graph = instantiate(*function, members, true);
    optimize(graph);
    SemanticSummary current = semantics(graph);
    const bool changed = current != previous[function];
    const bool first = evaluated.insert(function).second;
    summaries_[function] = std::move(graph);
    previous[function] = std::move(current);
    if (!changed && !first)
      continue;

    auto caller_it = callers.find(function);
    if (caller_it == callers.end())
      continue;
    for (const llvm::Function *caller : caller_it->second) {
      if (members.count(caller) != 0 && in_worklist.insert(caller).second)
        worklist.push_back(caller);
    }
  }
}

bool GPGAnalysisEngine::constructSummaries() {
  if (config_.mode == AnalysisMode::FlowAndContextInsensitive) {
    constructFlowInsensitiveSummaries(false);
    return true;
  }
  if (config_.mode == AnalysisMode::FlowInsensitiveContextSensitive) {
    constructFlowInsensitiveSummaries(true);
    return true;
  }

  auto callees = buildCallees();
  auto callers = buildCallers(callees);
  std::vector<SCC> sccs = computeSCCs(callees);

  for (const SCC &scc : sccs)
    constructSCC(scc, callees, callers);
  return true;
}

GPUSet GPGAnalysisEngine::graphGPUs(const GPG &graph) const {
  GPUSet result = graph.supportGPUs();
  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    result.insert(block.gpus.begin(), block.gpus.end());
  }
  return result;
}

GPUSet GPGAnalysisEngine::solveFlowInsensitive(const GPUSet &input) const {
  GPUSet closure = input;
  std::deque<GPU> worklist(input.begin(), input.end());
  while (!worklist.empty()) {
    GPU consumer = worklist.front();
    worklist.pop_front();
    ReductionResult reduced =
        reduceGPU(consumer, closure, closure, config_.heap_indirection_limit);
    for (GPU gpu : reduced.reduced) {
      // Flow-insensitive stores never kill an earlier relation.
      gpu.source_may_alias_multiple = true;
      if (closure.insert(gpu).second)
        worklist.push_back(std::move(gpu));
    }
  }
  return closure;
}

GPG GPGAnalysisEngine::makeFlowInsensitiveSummary(GPUSet gpus) const {
  GPG graph;
  GPB store;
  store.id = 1;
  store.kind = GPBKind::Start;
  store.gpus = std::move(gpus);
  store.original_gpus = store.gpus;
  store.may_definitions = gpuSources(store.gpus);
  graph.addBlock(std::move(store));
  graph.setEntry(1);
  graph.setExit(1);
  return graph;
}

GPUSet GPGAnalysisEngine::instantiateFlowInsensitiveCall(
    const llvm::CallBase &call, const llvm::Function &callee,
    const GPUSet &callee_summary) {
  GPG dummy = makeFlowInsensitiveSummary(callee_summary);
  CallExpansion expansion = makeExpansion(call, callee, dummy);

  auto clone_access = [&](Access access) {
    const MemoryLocation *location = model_.getLocation(access.location);
    if (!location || location->owner != &callee)
      return access;
    access.location = model_.contextLocation(&call, access.location);
    access.type = model_.typeAt(access.location, access.indirections);
    return access;
  };
  auto clone_gpu = [&](GPU gpu) {
    gpu.source = clone_access(gpu.source);
    gpu.target = clone_access(gpu.target);
    std::set<GPUQuery> queries;
    for (GPUQuery query : gpu.queries) {
      query.original = clone_access(query.original);
      queries.insert(std::move(query));
    }
    gpu.queries = std::move(queries);
    return gpu;
  };

  GPUSet instance;
  for (const GPU &gpu : callee_summary)
    instance.insert(clone_gpu(gpu));
  for (const GPU &gpu : expansion.parameter_gpus)
    instance.insert(clone_gpu(gpu));
  for (const GPU &gpu : expansion.return_gpus)
    instance.insert(clone_gpu(gpu));
  return solveFlowInsensitive(instance);
}

void GPGAnalysisEngine::constructFlowInsensitiveSummaries(
    bool context_sensitive) {
  if (!context_sensitive) {
    GPUSet program;
    for (const auto &[function, graph] : initial_graphs_) {
      (void)function;
      GPUSet local = graphGPUs(graph);
      program.insert(local.begin(), local.end());
      for (const auto &[id, block] : graph.blocks()) {
        (void)id;
        if (!block.callsite)
          continue;
        TargetSet targets(block.callees.begin(), block.callees.end());
        auto indirect = indirect_targets_.find(block.callsite);
        if (indirect != indirect_targets_.end())
          targets.insert(indirect->second.begin(), indirect->second.end());
        for (const llvm::Function *target : targets) {
          auto summary_it = summaries_.find(target);
          GPUSet callee_gpus = summary_it == summaries_.end()
                                   ? GPUSet{}
                                   : graphGPUs(summary_it->second);
          GPUSet mapping = instantiateFlowInsensitiveCall(*block.callsite,
                                                          *target, callee_gpus);
          program.insert(mapping.begin(), mapping.end());
        }
      }
    }
    program.insert(frontend_.globalInitializers().begin(),
                   frontend_.globalInitializers().end());
    GPUSet solved = solveFlowInsensitive(program);
    for (const auto &[function, graph] : initial_graphs_) {
      (void)graph;
      summaries_[function] = makeFlowInsensitiveSummary(solved);
    }
    observed_gpus_.insert(solved.begin(), solved.end());
    return;
  }

  std::map<const llvm::Function *, GPUSet> function_summaries;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[function, graph] : initial_graphs_) {
      GPUSet input = graphGPUs(graph);
      for (const auto &[id, block] : graph.blocks()) {
        (void)id;
        if (!block.callsite)
          continue;
        TargetSet targets(block.callees.begin(), block.callees.end());
        auto indirect = indirect_targets_.find(block.callsite);
        if (indirect != indirect_targets_.end())
          targets.insert(indirect->second.begin(), indirect->second.end());
        for (const llvm::Function *target : targets) {
          GPUSet callee_gpus;
          auto internal = function_summaries.find(target);
          if (internal != function_summaries.end()) {
            callee_gpus = internal->second;
          } else {
            auto external = summaries_.find(target);
            if (external != summaries_.end())
              callee_gpus = graphGPUs(external->second);
          }
          GPUSet instance = instantiateFlowInsensitiveCall(
              *block.callsite, *target, callee_gpus);
          input.insert(instance.begin(), instance.end());
        }
      }
      if (function->getName() == "main") {
        input.insert(frontend_.globalInitializers().begin(),
                     frontend_.globalInitializers().end());
      }
      GPUSet solved = solveFlowInsensitive(input);
      if (function_summaries[function] != solved) {
        function_summaries[function] = std::move(solved);
        changed = true;
      }
    }
  }

  for (auto &[function, gpus] : function_summaries) {
    observed_gpus_.insert(gpus.begin(), gpus.end());
    summaries_[function] = makeFlowInsensitiveSummary(std::move(gpus));
  }
}

bool GPGAnalysisEngine::signaturesCompatible(
    const llvm::CallBase &call, const llvm::Function &target) const {
  const llvm::FunctionType *call_type = call.getFunctionType();
  const llvm::FunctionType *target_type = target.getFunctionType();
  if (!call_type || !target_type)
    return true;
  if (!target_type->isVarArg() &&
      call_type->getNumParams() != target_type->getNumParams())
    return false;
  if (target_type->isVarArg() &&
      call_type->getNumParams() < target_type->getNumParams())
    return false;
  if (!model_.typesCompatible(call_type->getReturnType(),
                              target_type->getReturnType()))
    return false;
  const unsigned count =
      std::min(call_type->getNumParams(), target_type->getNumParams());
  for (unsigned index = 0; index < count; ++index) {
    if (!model_.typesCompatible(call_type->getParamType(index),
                                target_type->getParamType(index)))
      return false;
  }
  return true;
}

bool GPGAnalysisEngine::discoverIndirectTargets() {
  bool changed = false;
  for (const auto &[function, graph] : summaries_) {
    (void)function;
    for (const auto &[id, block] : graph.blocks()) {
      (void)id;
      for (const GPU &gpu : block.gpus) {
        const auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(gpu.origin);
        if (!call || !call->isIndirectCall() || !gpu.isUse() ||
            !gpu.target.indirections.empty())
          continue;
        const llvm::Function *target = model_.asFunction(gpu.target.location);
        if (!target || !signaturesCompatible(*call, *target))
          continue;
        changed |= indirect_targets_[call].insert(target).second;
      }
    }
  }
  if (changed)
    ++stats_.call_graph_refinements;
  return changed;
}

bool GPGAnalysisEngine::addFallbackIndirectTargets() {
  bool changed = false;
  for (const auto &[function, graph] : initial_graphs_) {
    (void)function;
    for (const auto &[id, block] : graph.blocks()) {
      (void)id;
      if (block.kind != GPBKind::IndirectCall || !block.callsite ||
          !indirect_targets_[block.callsite].empty())
        continue;
      for (llvm::Function &candidate : module_) {
        if (candidate.isIntrinsic() || !candidate.hasAddressTaken() ||
            !signaturesCompatible(*block.callsite, candidate))
          continue;
        changed |= indirect_targets_[block.callsite].insert(&candidate).second;
      }
    }
  }
  if (changed)
    ++stats_.call_graph_refinements;
  return changed;
}

void GPGAnalysisEngine::collectResult() {
  result_.clear();
  for (const MemoryLocation &location : model_.locations()) {
    if (location.id == 0)
      continue;
    result_.registerLocation(location.id, location.value, location.name);
    if (location.value &&
        model_.locationForValue(location.value) == location.id)
      result_.registerValue(location.value, location.id);
  }

  for (const GPU &gpu : observed_gpus_)
    result_.recordGPU(gpu);
  for (const auto &[function, graph] : summaries_) {
    GPGStats graph_stats = graph.stats();
    stats_.optimized_gpbs += graph_stats.blocks;
    stats_.optimized_gpus += graph_stats.gpus;
    for (const auto &[id, block] : graph.blocks()) {
      (void)id;
      for (const GPU &gpu : block.gpus)
        result_.recordGPU(gpu);
    }
    for (const GPU &gpu : graph.supportGPUs())
      result_.recordGPU(gpu);

    ModRefSummary mod_ref;
    SemanticSummary effect = semantics(graph);
    GPUSet exit_gpus = effect.first;
    exit_gpus.insert(effect.second.begin(), effect.second.end());
    for (const GPU &gpu : exit_gpus) {
      if (!gpu.isUse() && !gpu.isBoundary() && gpu.kind != GPUKind::Parameter)
        mod_ref.modifications.insert(gpu.source);
      if (!gpu.target.indirections.empty())
        mod_ref.references.insert(gpu.target);
      if (gpu.source.indirections.size() > 1)
        mod_ref.references.insert(gpu.source);
    }
    result_.setModRefSummary(function, std::move(mod_ref));
  }

  std::set<const llvm::CallBase *> resolved_calls;
  for (const auto &[call, targets] : indirect_targets_) {
    for (const llvm::Function *target : targets)
      result_.recordCallTarget(call, target);
    if (!targets.empty())
      resolved_calls.insert(call);
  }
  stats_.resolved_indirect_calls = resolved_calls.size();
  stats_.indirect_calls = indirect_targets_.size();
  result_.setStats(stats_);
}

void GPGAnalysisEngine::run() {
  result_.clear();
  indirect_targets_.clear();
  observed_gpus_.clear();
  buildInitialGraphs();

  while (true) {
    for (auto iterator = summaries_.begin(); iterator != summaries_.end();) {
      if (isDefinedFunction(iterator->first))
        iterator = summaries_.erase(iterator);
      else
        ++iterator;
    }
    constructSummaries();
    if (!discoverIndirectTargets())
      break;
  }

  if (addFallbackIndirectTargets()) {
    while (true) {
      for (auto iterator = summaries_.begin(); iterator != summaries_.end();) {
        if (isDefinedFunction(iterator->first))
          iterator = summaries_.erase(iterator);
        else
          ++iterator;
      }
      constructSummaries();
      if (!discoverIndirectTargets())
        break;
    }
  }
  collectResult();
}

char GPGAnalysisPass::ID = 0;

GPGAnalysisPass::GPGAnalysisPass(GPGConfig config)
    : llvm::ModulePass(ID), config_(std::move(config)) {}

bool GPGAnalysisPass::runOnModule(llvm::Module &module) {
  engine_ = std::make_unique<GPGAnalysisEngine>(module, config_);
  engine_->run();
  return false;
}

void GPGAnalysisPass::getAnalysisUsage(llvm::AnalysisUsage &usage) const {
  usage.setPreservesAll();
}

const GPGResult &GPGAnalysisPass::getResult() const {
  static const GPGResult EMPTY_RESULT;
  return engine_ ? engine_->result() : EMPTY_RESULT;
}

static llvm::RegisterPass<GPGAnalysisPass>
    X("lotus-gpg", "GPG flow- and context-sensitive pointer analysis", false,
      true);

} // namespace lotus::gpg
