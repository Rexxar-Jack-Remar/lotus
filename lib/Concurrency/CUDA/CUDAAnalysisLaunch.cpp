#include "CUDAAnalysisInternal.h"
#include "Concurrency/CUDA/CUDAAnalysis.h"
#include "Concurrency/CUDA/CUDAFunctionSummary.h"
#include "Concurrency/CUDA/CUDAKernelProtocolAnalysis.h"
#include "Concurrency/CUDA/CUDAParticipantAnalysis.h"
#include "Concurrency/CUDA/CUDAStreamAutomaton.h"

#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace concurrency::cuda {

namespace detail {

static void addOrderedDependency(llvm::SmallVectorImpl<size_t> &deps,
                                 size_t dep) {
  if (!llvm::is_contained(deps, dep)) {
    deps.push_back(dep);
  }
}

static void addOrderedDependencies(llvm::SmallVectorImpl<size_t> &deps,
                                   llvm::ArrayRef<size_t> more) {
  for (size_t dep : more) {
    addOrderedDependency(deps, dep);
  }
}

static bool isLegacyConfigureCall(const CallBase *call) {
  const Function *callee = call ? call->getCalledFunction() : nullptr;
  return callee && callee->getName().contains("cudaConfigureCall");
}

bool isNVVMKernel(const Function *function) {
  return function && (function->hasFnAttribute("nvvm.kernel") ||
                      function->getCallingConv() == CallingConv::PTX_Kernel);
}

bool isCUDAKernelCandidate(const Function *function) {
  return function && !function->isDeclaration();
}

const Value *getStreamOperand(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return nullptr;
  }
  const Function *callee = call->getCalledFunction();
  if (!callee) {
    return nullptr;
  }
  StringRef name = callee->getName();
  if (name.contains("cudaMemcpyAsync") || name.contains("cudaMemsetAsync")) {
    return call->arg_size() >= 4 ? call->getArgOperand(call->arg_size() - 1)
                                 : nullptr;
  }
  if (name.contains("cudaMemPrefetchAsync")) {
    return call->arg_size() >= 4 ? call->getArgOperand(3) : nullptr;
  }
  if (name.contains("cudaStream")) {
    return call->getArgOperand(0);
  }
  if (name.contains("cudaEventRecord")) {
    return call->arg_size() >= 2 ? call->getArgOperand(1) : nullptr;
  }
  if (name.contains("cudaStreamWaitEvent")) {
    return call->arg_size() >= 1 ? call->getArgOperand(0) : nullptr;
  }
  if (name.contains("cudaLaunchKernel")) {
    return call->arg_size() >= 9 ? call->getArgOperand(8) : nullptr;
  }
  return nullptr;
}

const Value *getEventOperand(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return nullptr;
  }
  const Function *callee = call->getCalledFunction();
  if (!callee) {
    return nullptr;
  }
  StringRef name = callee->getName();
  if (name.contains("cudaEvent")) {
    return call->getArgOperand(0);
  }
  if (name.contains("cudaStreamWaitEvent")) {
    return call->arg_size() >= 2 ? call->getArgOperand(1) : nullptr;
  }
  return nullptr;
}

const Value *getPotentialStream(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return nullptr;
  }
  if (const Value *stream = getStreamOperand(call)) {
    return stream;
  }
  ThreadAPI *thread_api = ThreadAPI::getThreadAPI();
  if (thread_api &&
      thread_api->getType(call) == ThreadAPI::TD_CUDA_KERNEL_LAUNCH &&
      call->arg_size() <= 6) {
    return nullptr;
  }
  return call->getArgOperand(call->arg_size() - 1);
}

HostStreamKind classifyHostStream(const CallBase *call, const Value *stream) {
  if (!call) {
    return HostStreamKind::Unknown;
  }
  ThreadAPI *thread_api = ThreadAPI::getThreadAPI();
  const bool is_runtime_launch =
      thread_api && thread_api->getType(call) == ThreadAPI::TD_CUDA_KERNEL_LAUNCH &&
      call->getCalledFunction() &&
      call->getCalledFunction()->getName().contains("cudaLaunchKernel");
  if (!stream) {
    return is_runtime_launch ? HostStreamKind::LegacyDefault
                             : HostStreamKind::Unknown;
  }
  if (const auto *constant = dyn_cast<Constant>(stream)) {
    if (constant->isNullValue()) {
      return is_runtime_launch ? HostStreamKind::LegacyDefault
                               : HostStreamKind::Unknown;
    }
    return HostStreamKind::Explicit;
  }
  const Value *base = stream->stripPointerCasts();
  if (const auto *int_to_ptr = dyn_cast<IntToPtrInst>(base)) {
    if (const auto *ci = dyn_cast<ConstantInt>(int_to_ptr->getOperand(0))) {
      if (ci->isZero()) {
        return is_runtime_launch ? HostStreamKind::LegacyDefault
                                 : HostStreamKind::Unknown;
      }
      return HostStreamKind::Explicit;
    }
  }
  return isa<Argument>(base) || isa<GlobalValue>(base)
             ? HostStreamKind::Explicit
             : HostStreamKind::Unknown;
}

LaunchOrderingSource getOrderingSource(ThreadAPI::TD_TYPE type) {
  switch (type) {
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
    return LaunchOrderingSource::DeviceSynchronize;
  case ThreadAPI::TD_CUDA_STREAM:
    return LaunchOrderingSource::StreamSynchronize;
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
    return LaunchOrderingSource::MemoryBarrier;
  default:
    return LaunchOrderingSource::Unknown;
  }
}

SynchronizationScope getSyncScope(ThreadAPI::TD_TYPE type) {
  switch (type) {
  case ThreadAPI::TD_CUDA_WARP_BARRIER:
    return SynchronizationScope::Warp;
  case ThreadAPI::TD_CUDA_BARRIER:
    return SynchronizationScope::Block;
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
    return SynchronizationScope::Device;
  case ThreadAPI::TD_CUDA_STREAM:
  case ThreadAPI::TD_CUDA_EVENT:
    return SynchronizationScope::Device;
  default:
    return SynchronizationScope::None;
  }
}

SynchronizationPrimitive getSynchronizationPrimitive(ThreadAPI::TD_TYPE type,
                                                     const Instruction *inst) {
  switch (type) {
  case ThreadAPI::TD_CUDA_WARP_BARRIER:
    return SynchronizationPrimitive::WarpBarrier;
  case ThreadAPI::TD_CUDA_BARRIER:
    return SynchronizationPrimitive::BlockBarrier;
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
    return SynchronizationPrimitive::DeviceSynchronize;
  case ThreadAPI::TD_CUDA_STREAM:
  case ThreadAPI::TD_CUDA_EVENT:
    return SynchronizationPrimitive::StreamProgramOrder;
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
    if (const auto *call = dyn_cast_or_null<CallBase>(inst)) {
      if (const Function *callee = call->getCalledFunction()) {
        StringRef name = callee->getName();
        if (name.contains("threadfence_block") || name.contains("membar.cta")) {
          return SynchronizationPrimitive::BlockFence;
        }
        if (name.contains("threadfence_system") ||
            name.contains("membar.sys")) {
          return SynchronizationPrimitive::SystemFence;
        }
      }
    }
    return SynchronizationPrimitive::DeviceFence;
  default:
    return SynchronizationPrimitive::None;
  }
}

bool launchesOrdered(const std::vector<KernelLaunchInfo> &launches,
                     size_t earlier_idx, size_t later_idx) {
  if (earlier_idx >= later_idx || later_idx >= launches.size()) {
    return false;
  }

  const KernelLaunchInfo &earlier = launches[earlier_idx];
  const KernelLaunchInfo &later = launches[later_idx];
  if (llvm::is_contained(later.ordered_dependencies, earlier_idx)) {
    return true;
  }
  for (size_t mid = earlier_idx + 1; mid <= later_idx; ++mid) {
    const KernelLaunchInfo &cur = launches[mid];
    if (llvm::is_contained(cur.ordered_dependencies, earlier_idx)) {
      return true;
    }
    if (cur.ordering_source == LaunchOrderingSource::DeviceSynchronize) {
      return true;
    }
  }

  const bool same_stream =
      earlier.stream_kind == HostStreamKind::Explicit &&
      later.stream_kind == HostStreamKind::Explicit &&
      earlier.stream_known && later.stream_known &&
      earlier.stream == later.stream;
  if (same_stream) {
    return true;
  }
  return earlier.stream_kind == HostStreamKind::LegacyDefault &&
         later.stream_kind == HostStreamKind::Explicit;
}

detail::LaunchOrderingState::StreamState *
getMutableStreamState(detail::LaunchOrderingState &ordering_state,
                      HostStreamKind stream_kind, const Value *stream) {
  if (stream_kind == HostStreamKind::Explicit && stream) {
    return &ordering_state.stream_states[stream];
  }
  if (stream_kind == HostStreamKind::LegacyDefault) {
    return &ordering_state.default_stream;
  }
  return nullptr;
}

static detail::LaunchOrderingState::StreamState &
getHostState(detail::LaunchOrderingState &ordering_state,
             HostStreamKind stream_kind, const Value *stream) {
  if (auto *stream_state =
          getMutableStreamState(ordering_state, stream_kind, stream)) {
    return *stream_state;
  }
  return ordering_state.host_state;
}

const detail::LaunchOrderingState::StreamState *
getStreamState(const detail::LaunchOrderingState &ordering_state,
               HostStreamKind stream_kind, const Value *stream) {
  if (stream_kind == HostStreamKind::Explicit && stream) {
    auto it = ordering_state.stream_states.find(stream);
    return it == ordering_state.stream_states.end() ? nullptr : &it->second;
  }
  if (stream_kind == HostStreamKind::LegacyDefault) {
    return &ordering_state.default_stream;
  }
  return nullptr;
}

void markStreamOrdered(detail::LaunchOrderingState &ordering_state,
                       HostStreamKind stream_kind, const Value *stream,
                       SynchronizationScope scope,
                       LaunchOrderingSource source,
                       SynchronizationPrimitive primitive) {
  auto &stream_state = getHostState(ordering_state, stream_kind, stream);
  stream_state.ordered_since_last_launch = true;
  stream_state.usable_for_unknown_launch =
      stream_kind != HostStreamKind::Unknown;
  stream_state.scope = scope;
  stream_state.source = source;
  stream_state.primitive = primitive;
  stream_state.stream = stream;
  stream_state.stream_kind = stream_kind;
}

void clearStreamOrdered(detail::LaunchOrderingState &ordering_state,
                        HostStreamKind stream_kind, const Value *stream) {
  auto &stream_state = getHostState(ordering_state, stream_kind, stream);
  stream_state.ordered_since_last_launch = false;
  stream_state.usable_for_unknown_launch = false;
  stream_state.scope = SynchronizationScope::None;
  stream_state.source = LaunchOrderingSource::None;
  stream_state.primitive = SynchronizationPrimitive::None;
  stream_state.stream = nullptr;
  stream_state.stream_kind = HostStreamKind::Unknown;
  stream_state.ordered_dependencies.clear();
}

} // namespace detail

bool LaunchDimensions::hasSymbolicGrid() const {
  return llvm::any_of(grid, [](const SymbolicDimension &dim) {
    return dim.kind != SymbolicValueKind::Constant;
  });
}

bool LaunchDimensions::hasSymbolicBlock() const {
  return llvm::any_of(block, [](const SymbolicDimension &dim) {
    return dim.kind != SymbolicValueKind::Constant;
  });
}

CUDAAnalysis::CUDAAnalysis(Module &module,
                           lotus::AliasAnalysisWrapper *alias_analysis,
                           DeviceConfig config)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()),
      m_alias_analysis(alias_analysis), m_device_config(config) {
  if (!m_alias_analysis) {
    initializeDefaultAliasAnalysis();
  }
}

CUDAAnalysis::CUDAAnalysis(Module &module, DeviceConfig config)
    : CUDAAnalysis(module, nullptr, config) {}

void CUDAAnalysis::runAnalysis() {
  m_launches.clear();
  m_kernel_summaries.clear();
  m_kernel_index.clear();
  m_inter_kernel_races.clear();
  m_memory_transfers.clear();
  m_unified_memory.clear();
  m_abstract_state.clear();
  size_t launch_sequence = 0;
  CUDASteamAutomatonBuilder automaton_builder(m_abstract_state);

  for (Function &function : m_module) {
    if (function.isDeclaration()) {
      continue;
    }

    detail::LaunchOrderingState ordering_state;
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      const Function *callee = m_thread_api->getCallee(call);
      ThreadAPI::TD_TYPE type =
          callee ? m_thread_api->getType(callee) : ThreadAPI::TD_DUMMY;

      if (type == ThreadAPI::TD_CUDA_DEVICE_SYNC ||
          type == ThreadAPI::TD_CUDA_MEMORY_BARRIER) {
        ordering_state.device_synchronized =
            type == ThreadAPI::TD_CUDA_DEVICE_SYNC;
        const Value *stream = detail::getPotentialStream(call);
        const HostStreamKind stream_kind =
            detail::classifyHostStream(call, stream);
        auto &host_state =
            detail::getHostState(ordering_state, stream_kind, stream);
        host_state.ordered_dependencies.clear();
        for (size_t dep = 0; dep < m_launches.size(); ++dep) {
          detail::addOrderedDependency(host_state.ordered_dependencies, dep);
        }
        detail::markStreamOrdered(ordering_state, stream_kind, stream,
                                  detail::getSyncScope(type),
                                  detail::getOrderingSource(type),
                                  detail::getSynchronizationPrimitive(type,
                                                                      &inst));
        if (type == ThreadAPI::TD_CUDA_DEVICE_SYNC) {
          automaton_builder.addDeviceSync(&inst);
        } else if (stream_kind != HostStreamKind::Unknown) {
          automaton_builder.addStreamSync(&inst, stream);
        }
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_STREAM) {
        const Value *stream = detail::getStreamOperand(call);
        const HostStreamKind stream_kind =
            detail::classifyHostStream(call, stream);
        const Value *event = detail::getEventOperand(call);
        const Function *called_fn = call->getCalledFunction();
        StringRef name = called_fn ? called_fn->getName() : StringRef{};
        if (name.contains("Create")) {
          automaton_builder.addStream(stream);
        } else if (name.contains("Destroy")) {
          automaton_builder.addStreamDestroy(&inst, stream);
        } else if (name.contains("Synchronize")) {
          auto &host_state =
              detail::getHostState(ordering_state, stream_kind, stream);
          host_state.ordered_dependencies.clear();
          for (size_t dep = 0; dep < m_launches.size(); ++dep) {
            detail::addOrderedDependency(host_state.ordered_dependencies, dep);
          }
          detail::markStreamOrdered(ordering_state, stream_kind, stream,
                                    SynchronizationScope::Device,
                                    LaunchOrderingSource::StreamSynchronize,
                                    SynchronizationPrimitive::StreamProgramOrder);
          ordering_state.host_state = host_state;
          if (stream_kind != HostStreamKind::Unknown) {
            automaton_builder.addStreamSync(&inst, stream);
          }
        } else if (name.contains("WaitEvent")) {
          automaton_builder.addEventWait(&inst, event, stream);
          auto event_it = ordering_state.event_states.find(event);
          if (event_it != ordering_state.event_states.end() &&
              event_it->second.has_record &&
              stream_kind == HostStreamKind::Explicit && stream) {
            auto &host_state =
                detail::getHostState(ordering_state, stream_kind, stream);
            host_state.ordered_dependencies.clear();
            detail::addOrderedDependencies(
                host_state.ordered_dependencies,
                event_it->second.recorded_dependencies);
            detail::markStreamOrdered(
                ordering_state, stream_kind, stream, SynchronizationScope::Device,
                LaunchOrderingSource::StreamSynchronize,
                SynchronizationPrimitive::StreamProgramOrder);
            ordering_state.host_state = host_state;
          }
        }
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_EVENT) {
        const Value *event = detail::getEventOperand(call);
        const Value *stream = detail::getStreamOperand(call);
        const HostStreamKind stream_kind =
            detail::classifyHostStream(call, stream);
        const Function *called_fn = call->getCalledFunction();
        StringRef name = called_fn ? called_fn->getName() : StringRef{};
        if (name.contains("Record")) {
          if (event) {
            auto &event_state = ordering_state.event_states[event];
            event_state.has_record = true;
            event_state.recorded_stream = stream;
            event_state.recorded_stream_kind = stream_kind;
            event_state.recorded_dependencies.clear();
            for (size_t dep = 0; dep < m_launches.size(); ++dep) {
              detail::addOrderedDependency(event_state.recorded_dependencies,
                                           dep);
            }
          }
          automaton_builder.addEvent(&inst, event, stream);
        } else if (name.contains("Wait")) {
          automaton_builder.addEventWait(&inst, event, stream);
          auto event_it = ordering_state.event_states.find(event);
          if (event_it != ordering_state.event_states.end() &&
              event_it->second.has_record &&
              stream_kind == HostStreamKind::Explicit && stream) {
            auto &host_state =
                detail::getHostState(ordering_state, stream_kind, stream);
            host_state.ordered_dependencies.clear();
            detail::addOrderedDependencies(
                host_state.ordered_dependencies,
                event_it->second.recorded_dependencies);
            detail::markStreamOrdered(
                ordering_state, stream_kind, stream, SynchronizationScope::Device,
                LaunchOrderingSource::StreamSynchronize,
                SynchronizationPrimitive::StreamProgramOrder);
          }
        } else if (name.contains("Synchronize")) {
          if (event) {
            auto event_it = ordering_state.event_states.find(event);
            if (event_it != ordering_state.event_states.end() &&
                event_it->second.has_record) {
              ordering_state.host_state.ordered_dependencies.clear();
              detail::addOrderedDependencies(
                  ordering_state.host_state.ordered_dependencies,
                  event_it->second.recorded_dependencies);
              detail::markStreamOrdered(
                  ordering_state, HostStreamKind::Unknown, nullptr,
                  SynchronizationScope::Device,
                  LaunchOrderingSource::StreamSynchronize,
                  SynchronizationPrimitive::StreamProgramOrder);
              ordering_state.host_state.usable_for_unknown_launch = true;
            }
          }
          automaton_builder.addEventSync(&inst, event);
        }
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_MEMCPY || type == ThreadAPI::TD_CUDA_MEMSET ||
          type == ThreadAPI::TD_CUDA_UNIFIED_MEMORY) {
        const Function *called_fn = call->getCalledFunction();
        if (called_fn && called_fn->getName().contains("Async")) {
          const Value *stream = detail::getStreamOperand(call);
          const HostStreamKind stream_kind =
              detail::classifyHostStream(call, stream);
          if (stream_kind != HostStreamKind::Unknown) {
            auto &host_state =
                detail::getHostState(ordering_state, stream_kind, stream);
            host_state.ordered_dependencies.clear();
            for (size_t dep = 0; dep < m_launches.size(); ++dep) {
              detail::addOrderedDependency(host_state.ordered_dependencies, dep);
            }
            detail::markStreamOrdered(ordering_state, stream_kind, stream,
                                      SynchronizationScope::Device,
                                      LaunchOrderingSource::ProgramOrder,
                                      SynchronizationPrimitive::StreamProgramOrder);
            ordering_state.host_state = host_state;
          }
          if (stream) {
            automaton_builder.addStream(stream);
            automaton_builder.addEvent(&inst, nullptr, stream);
          }
        }
      }

      if (type != ThreadAPI::TD_CUDA_KERNEL_LAUNCH) {
        continue;
      }

      if (detail::isLegacyConfigureCall(call)) {
        continue;
      }

      const Function *kernel = m_thread_api->getCUDALaunchedKernel(&inst);
      if (!detail::isCUDAKernelCandidate(kernel)) {
        recordModelGap(&inst, "CUDA launch site could not be matched to a "
                              "concrete kernel function",
                       0.35);
        continue;
      }

      const Value *stream = detail::getPotentialStream(call);
      const HostStreamKind stream_kind =
          detail::classifyHostStream(call, stream);
      const bool stream_known = stream_kind != HostStreamKind::Unknown;
      const auto *stream_state =
          detail::getStreamState(ordering_state, stream_kind, stream);
      const detail::LaunchOrderingState::StreamState *selected_state = nullptr;
      if (stream_known && stream_state &&
          stream_state->ordered_since_last_launch) {
        selected_state = stream_state;
      } else if (ordering_state.host_state.ordered_since_last_launch &&
                 ordering_state.host_state.usable_for_unknown_launch) {
        selected_state = &ordering_state.host_state;
      }
      KernelLaunchInfo launch;
      launch.launch = &inst;
      launch.dimensions = getLaunchDimensions(&inst);
      launch.sequence = launch_sequence++;
      launch.kernel = kernel;
      launch.stream = stream;
      launch.stream_known = stream_known;
      launch.stream_kind = stream_kind;
      launch.predecessor = SynchronizationPrimitive::None;
      launch.host_happens_before = false;

      if (ordering_state.device_synchronized) {
        launch.ordered_after_previous = true;
        launch.ordered_dependencies.clear();
        for (size_t dep = 0; dep < m_launches.size(); ++dep) {
          launch.ordered_dependencies.push_back(dep);
        }
        launch.ordering_scope = SynchronizationScope::Device;
        launch.ordering_source = LaunchOrderingSource::DeviceSynchronize;
        launch.predecessor = SynchronizationPrimitive::DeviceSynchronize;
        launch.host_happens_before = true;
      } else if (selected_state) {
        if (selected_state == &ordering_state.host_state && !stream_known) {
          launch.stream = selected_state->stream;
          launch.stream_kind = selected_state->stream_kind;
          launch.stream_known =
              selected_state->stream_kind != HostStreamKind::Unknown;
        }
        launch.ordered_after_previous = true;
        launch.ordered_dependencies = selected_state->ordered_dependencies;
        launch.ordering_scope = selected_state->scope;
        launch.ordering_source = selected_state->source;
        launch.predecessor = selected_state->primitive;
        launch.host_happens_before = true;
      } else if (!m_launches.empty()) {
        const KernelLaunchInfo &prev = m_launches.back();
        const bool same_stream =
            prev.stream_kind == HostStreamKind::Explicit &&
            stream_kind == HostStreamKind::Explicit && stream_known &&
            prev.stream_known && prev.stream == stream;
        const bool default_stream_orders =
            prev.stream_kind == HostStreamKind::LegacyDefault &&
            stream_kind == HostStreamKind::Explicit;
        if (same_stream || default_stream_orders) {
          launch.ordered_after_previous = true;
          launch.ordering_scope = SynchronizationScope::Device;
          launch.ordering_source = LaunchOrderingSource::ProgramOrder;
          launch.predecessor = SynchronizationPrimitive::StreamProgramOrder;
          launch.host_happens_before = true;
        }
      }

      m_launches.push_back(launch);
      if (!m_kernel_index.count(launch.kernel)) {
        analyzeKernel(launch.kernel, &m_launches.back());
      }
      if (launch.dimensions.hasSymbolicGrid() || launch.dimensions.hasSymbolicBlock()) {
        recordModelGap(&inst, "CUDA launch dimensions remain symbolic, "
                              "reducing precision for race and performance "
                              "diagnostics",
                       0.5);
      }
      detail::clearStreamOrdered(ordering_state, stream_kind, stream);
      ordering_state.device_synchronized = false;
    }
  }

  automaton_builder.finalize();

  for (Function &function : m_module) {
    if (function.isDeclaration() || !detail::isNVVMKernel(&function) ||
        m_kernel_index.count(&function)) {
      continue;
    }
    recordModelGap(&function, "Kernel analyzed without an explicit host-side "
                              "launch context; launch dimensions defaulted "
                              "conservatively",
                   0.55);
    analyzeKernel(&function, nullptr);
  }

  analyzeMemoryTransfers();
  analyzeUnifiedMemory();
  analyzeInterKernelRaces();

  CUDAFunctionSummaryAnalysis func_summary_analysis(m_module);
  func_summary_analysis.runAnalysis();

  for (const auto &pair : func_summary_analysis.getSummaries()) {
    m_abstract_state.function_summaries[pair.first] = pair.second;
  }

  size_t kernel_class_counter = 0;
  std::set<const llvm::Function *> analyzed_kernels;
  for (const auto &pair : m_kernel_index) {
    const llvm::Function *kernel = pair.first;
    if (!kernel)
      continue;
    analyzed_kernels.insert(kernel);
    CUDAParticipantAnalysis participant_analysis(*kernel);

    for (const auto &bb : *kernel) {
      for (const auto &inst : bb) {
        if (llvm::isa<llvm::CallBase>(&inst)) {
          auto set = participant_analysis.getActiveSetAt(&inst);
          if (!set.scopes.empty()) {
            set.kernel = kernel;
            set.instruction = &inst;
            m_abstract_state.participant_sets.push_back(set);
          }
        }
      }
    }

    CUDAKernelProtocolAnalysis protocol_analysis(*kernel, m_abstract_state);
    protocol_analysis.runAnalysis();
  }

  for (Function &function : m_module) {
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_thread_api->getType(call);
      if (type == ThreadAPI::TD_CUDA_DEVICE_SYNC) {
        CUDASynchronizationFact fact;
        fact.synchronization_class_id =
            m_abstract_state.synchronization_facts.size();
        fact.inst = call;
        fact.primitive =
            static_cast<int>(SynchronizationPrimitive::DeviceSynchronize);
        fact.scope = static_cast<int>(SynchronizationScope::Device);
        fact.ordering_effect = true;
        fact.participating_threads = 0;
        m_abstract_state.synchronization_facts.push_back(fact);
        m_abstract_state
            .synchronization_fact_by_class[fact.synchronization_class_id] =
            fact;
      }
    }
  }

  for (const auto &launch : m_launches) {
    if (!launch.kernel)
      continue;
    CUDAKernelFact fact;
    fact.kernel_class_id = kernel_class_counter++;
    fact.kernel = launch.kernel;
    fact.launch_site = launch.launch;
    fact.stream = launch.stream;
    fact.stream_known = launch.stream_known;
    fact.is_ordered_after_previous = launch.ordered_after_previous;
    m_abstract_state.kernel_facts.push_back(fact);
    m_abstract_state.kernel_fact_by_class[fact.kernel_class_id] = fact;
  }
}

void CUDAAnalysis::analyzeInterKernelRaces() {
  if (m_launches.size() < 2) {
    return;
  }

  for (size_t i = 0; i < m_launches.size(); ++i) {
    for (size_t j = i + 1; j < m_launches.size(); ++j) {
      const KernelLaunchInfo &launch_a = m_launches[i];
      const KernelLaunchInfo &launch_b = m_launches[j];

      if (!launch_a.kernel || !launch_b.kernel) {
        continue;
      }

      size_t idx_a = m_kernel_index[launch_a.kernel];
      size_t idx_b = m_kernel_index[launch_b.kernel];
      const KernelSummary &summary_a = m_kernel_summaries[idx_a];
      const KernelSummary &summary_b = m_kernel_summaries[idx_b];
      bool ordered = detail::launchesOrdered(m_launches, i, j);

      for (const AccessInfo &access_a : summary_a.accesses) {
        if (!access_a.base) {
          continue;
        }
        if (access_a.space != MemorySpace::Global &&
            access_a.space != MemorySpace::Device) {
          continue;
        }

        for (const AccessInfo &access_b : summary_b.accesses) {
          if (!access_b.base) {
            continue;
          }
          if (access_b.space != access_a.space &&
              !((access_a.space == MemorySpace::Global ||
                 access_a.space == MemorySpace::Device) &&
                (access_b.space == MemorySpace::Global ||
                 access_b.space == MemorySpace::Device))) {
            continue;
          }
          const detail::AliasQueryResult alias =
              detail::queryAlias(access_a, access_b, m_alias_analysis);
          if (alias.relation == AliasResult::NoAlias) {
            continue;
          }
          if ((access_a.is_write || access_b.is_write) && access_a.is_atomic &&
              access_b.is_atomic) {
            continue;
          }

          InterKernelRaceInfo race;
          race.first_launch = launch_a.launch;
          race.second_launch = launch_b.launch;
          race.first_kernel = launch_a.kernel;
          race.second_kernel = launch_b.kernel;
          race.shared_base = access_a.base;
          race.ordered = ordered;
          race.ordering_reason =
              race.ordered ? toString(launch_b.ordering_source) : "unordered";
          race.ordering_inst =
              launch_b.ordering_source != LaunchOrderingSource::None
                  ? launch_b.launch
                  : nullptr;
          race.ordering_source = launch_b.ordering_source;
          race.stream = launch_b.stream;
          race.stream_known = launch_b.stream_known;
          race.symbolic = launch_a.dimensions.hasSymbolicGrid() ||
                          launch_a.dimensions.hasSymbolicBlock() ||
                          launch_b.dimensions.hasSymbolicGrid() ||
                          launch_b.dimensions.hasSymbolicBlock();
          race.kind = (access_a.is_atomic || access_b.is_atomic)
                          ? RaceKind::AtomicOrderingRisk
                          : RaceKind::InterKernelHazard;
          race.alias_precision = alias.precision;
          race.alias_source = alias.source;
          race.missing_ordering = launch_b.predecessor;
          race.required_fence_scope = SynchronizationScope::Device;
          race.confidence =
              race.alias_precision == AliasPrecision::Exact ? 0.9 : 0.6;
          if (race.ordered) {
            continue;
          }
          m_inter_kernel_races.push_back(std::move(race));
        }
      }
    }
  }
}

LaunchDimensions CUDAAnalysis::getLaunchDimensions(const Instruction *launch) {
  LaunchDimensions dims;
  const auto *call = dyn_cast_or_null<CallBase>(launch);
  if (!call) {
    return dims;
  }

  for (unsigned i = 0; i < 3; ++i) {
    dims.grid[i].kind = SymbolicValueKind::Constant;
    dims.grid[i].constant = 1;
    dims.block[i].kind = SymbolicValueKind::Constant;
    dims.block[i].constant = 1;
  }

  if (call->arg_size() > 0) {
    dims.grid[0] = classifyDimension(call->getArgOperand(0));
  }
  if (call->arg_size() > 1) {
    dims.block[0] = classifyDimension(call->getArgOperand(1));
  }
  if (call->arg_size() > 2) {
    dims.grid[1] = classifyDimension(call->getArgOperand(2));
  }
  if (call->arg_size() > 3) {
    dims.block[1] = classifyDimension(call->getArgOperand(3));
  }
  if (call->arg_size() > 4) {
    dims.grid[2] = classifyDimension(call->getArgOperand(4));
  }
  if (call->arg_size() > 5) {
    dims.block[2] = classifyDimension(call->getArgOperand(5));
  }

  return dims;
}

void CUDAAnalysis::initializeDefaultAliasAnalysis() {
  m_owned_alias_analysis =
      lotus::AliasAnalysisFactory::createAserPTA(m_module, 0);
  if (m_owned_alias_analysis && m_owned_alias_analysis->isInitialized()) {
    m_alias_analysis = m_owned_alias_analysis.get();
    return;
  }
  m_owned_alias_analysis =
      lotus::AliasAnalysisFactory::createSparrowAA(m_module, 0);
  if (m_owned_alias_analysis && m_owned_alias_analysis->isInitialized()) {
    m_alias_analysis = m_owned_alias_analysis.get();
    return;
  }
  m_owned_alias_analysis.reset();
  m_alias_analysis = nullptr;
}

bool CUDAAnalysis::hasAliasAnalysis() const {
  return m_alias_analysis && m_alias_analysis->isInitialized();
}

} // namespace concurrency::cuda
