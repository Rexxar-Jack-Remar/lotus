#include "Concurrency/CUDA/CUDAFunctionSummary.h"

#include "Concurrency/Utils/ThreadAPI.h"

namespace concurrency::cuda {

namespace {

bool isCUDACall(ThreadAPI::TD_TYPE type) { return type != ThreadAPI::TD_DUMMY; }

} // anonymous namespace

CUDAFunctionSummaryAnalysis::CUDAFunctionSummaryAnalysis(
    const llvm::Module &module)
    : m_module(module) {}

void CUDAFunctionSummaryAnalysis::runAnalysis() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api) {
    return;
  }

  for (const auto &fn : m_module.functions()) {
    if (fn.isDeclaration()) {
      continue;
    }
    CUDAFunctionSummary summary;
    summary.function = &fn;

    for (const auto &bb : fn) {
      for (const auto &inst : bb) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call) {
          continue;
        }
        auto *callee = call->getCalledFunction();
        if (!callee) {
          continue;
        }

        auto td_type = api->getType(callee);
        if (isCUDACall(td_type)) {
          switch (td_type) {
          case ThreadAPI::TD_CUDA_KERNEL_LAUNCH:
            summary.kernel_launches.push_back(call);
            break;
          case ThreadAPI::TD_CUDA_MEMCPY:
          case ThreadAPI::TD_CUDA_MEMSET:
            summary.memory_transfers.push_back(call);
            break;
          case ThreadAPI::TD_CUDA_DEVICE_SYNC:
          case ThreadAPI::TD_CUDA_BARRIER:
          case ThreadAPI::TD_CUDA_WARP_BARRIER:
          case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
            summary.synchronizations.push_back(call);
            break;
          case ThreadAPI::TD_CUDA_ATOMIC:
            summary.atomics.push_back(call);
            break;
          default:
            break;
          }
        }

        if (!callee->isDeclaration()) {
          bool already_added = false;
          for (const llvm::Function *existing : summary.callees) {
            if (existing == callee) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.callees.push_back(callee);
          }
        }
      }
    }

    m_summaries[&fn] = summary;
  }

  bool changed = true;
  int max_iterations = 10;
  int iteration = 0;

  while (changed && iteration < max_iterations) {
    changed = false;
    ++iteration;

    for (auto &pair : m_summaries) {
      CUDAFunctionSummary &summary = pair.second;
      size_t orig_kernel_count = summary.kernel_launches.size();
      size_t orig_transfer_count = summary.memory_transfers.size();
      size_t orig_sync_count = summary.synchronizations.size();
      size_t orig_atomic_count = summary.atomics.size();

      for (const llvm::Function *callee : summary.callees) {
        auto it = m_summaries.find(callee);
        if (it == m_summaries.end()) {
          continue;
        }
        const CUDAFunctionSummary &callee_summary = it->second;

        for (const auto *call : callee_summary.kernel_launches) {
          bool already_added = false;
          for (const auto *existing : summary.kernel_launches) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.kernel_launches.push_back(call);
          }
        }

        for (const auto *call : callee_summary.memory_transfers) {
          bool already_added = false;
          for (const auto *existing : summary.memory_transfers) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.memory_transfers.push_back(call);
          }
        }

        for (const auto *call : callee_summary.synchronizations) {
          bool already_added = false;
          for (const auto *existing : summary.synchronizations) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.synchronizations.push_back(call);
          }
        }

        for (const auto *call : callee_summary.atomics) {
          bool already_added = false;
          for (const auto *existing : summary.atomics) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.atomics.push_back(call);
          }
        }

        if (callee_summary.recursive) {
          summary.recursive = true;
        }
      }

      if (summary.kernel_launches.size() != orig_kernel_count ||
          summary.memory_transfers.size() != orig_transfer_count ||
          summary.synchronizations.size() != orig_sync_count ||
          summary.atomics.size() != orig_atomic_count) {
        changed = true;
      }
    }
  }

  for (auto &pair : m_summaries) {
    CUDAFunctionSummary &summary = pair.second;

    std::set<const llvm::Function *> visited;
    visited.insert(summary.function);

    bool has_cycle = false;
    std::function<void(const llvm::Function *,
                       std::set<const llvm::Function *> &)>
        detect_recursion = [&](const llvm::Function *fn,
                               std::set<const llvm::Function *> &vis) {
          auto it = m_summaries.find(fn);
          if (it == m_summaries.end()) {
            return;
          }
          const CUDAFunctionSummary &fn_summary = it->second;
          for (const llvm::Function *callee : fn_summary.callees) {
            if (callee->isDeclaration()) {
              continue;
            }
            if (vis.count(callee)) {
              has_cycle = true;
              return;
            }
            vis.insert(callee);
            detect_recursion(callee, vis);
            if (has_cycle) {
              return;
            }
            vis.erase(callee);
          }
        };

    std::set<const llvm::Function *> local_visited;
    detect_recursion(summary.function, local_visited);

    summary.recursive = has_cycle;
  }

  for (auto &pair : m_summaries) {
    pair.second.reaches_fixed_point = (iteration < max_iterations);
  }
}

const CUDAFunctionSummary *
CUDAFunctionSummaryAnalysis::getSummary(const llvm::Function *fn) const {
  auto it = m_summaries.find(fn);
  if (it != m_summaries.end()) {
    return &it->second;
  }
  return nullptr;
}

} // namespace concurrency::cuda