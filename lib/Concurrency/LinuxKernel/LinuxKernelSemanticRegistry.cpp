/**
 * @file LinuxKernelSemanticRegistry.cpp
 * @brief Declarative Linux kernel API semantic registry implementation.
 */

#include "Concurrency/LinuxKernel/LinuxKernelSemanticRegistry.h"

#include "Concurrency/LinuxKernel/LinuxKernelConfig.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace kernel {
namespace {

template <typename Enum>
bool parseEnum(StringRef value,
               const std::unordered_map<std::string, Enum> &mapping,
               Enum &result) {
  auto found = mapping.find(value.lower());
  if (found == mapping.end()) {
    return false;
  }
  result = found->second;
  return true;
}

bool parseOperation(StringRef value, OperationKind &result) {
  static const std::unordered_map<std::string, OperationKind> operations = {
#define LOTUS_KERNEL_OPERATION(Name)                                           \
  {StringRef(#Name).lower(), OperationKind::Name}
      LOTUS_KERNEL_OPERATION(LOCK_ACQUIRE),
      LOTUS_KERNEL_OPERATION(LOCK_RELEASE),
      LOTUS_KERNEL_OPERATION(LOCK_TRY),
      LOTUS_KERNEL_OPERATION(LOCK_INIT),
      LOTUS_KERNEL_OPERATION(RCU_READ_LOCK),
      LOTUS_KERNEL_OPERATION(RCU_READ_UNLOCK),
      LOTUS_KERNEL_OPERATION(RCU_SYNC),
      LOTUS_KERNEL_OPERATION(RCU_CALL),
      LOTUS_KERNEL_OPERATION(RCU_ASSIGN),
      LOTUS_KERNEL_OPERATION(RCU_DEREFERENCE),
      LOTUS_KERNEL_OPERATION(RCU_RECLAIM),
      LOTUS_KERNEL_OPERATION(RCU_BARRIER),
      LOTUS_KERNEL_OPERATION(SEQLOCK_INIT),
      LOTUS_KERNEL_OPERATION(SEQ_READ_BEGIN),
      LOTUS_KERNEL_OPERATION(SEQ_READ_RETRY),
      LOTUS_KERNEL_OPERATION(SEQ_WRITE_LOCK),
      LOTUS_KERNEL_OPERATION(SEQ_WRITE_UNLOCK),
      LOTUS_KERNEL_OPERATION(COMPLETION_WAIT),
      LOTUS_KERNEL_OPERATION(COMPLETION_SIGNAL),
      LOTUS_KERNEL_OPERATION(COMPLETION_INIT),
      LOTUS_KERNEL_OPERATION(COMPLETION_REINIT),
      LOTUS_KERNEL_OPERATION(WAITQUEUE_INIT),
      LOTUS_KERNEL_OPERATION(WAIT_EVENT),
      LOTUS_KERNEL_OPERATION(WAKE_UP),
      LOTUS_KERNEL_OPERATION(PREPARE_WAIT),
      LOTUS_KERNEL_OPERATION(FINISH_WAIT),
      LOTUS_KERNEL_OPERATION(TIMER_SETUP),
      LOTUS_KERNEL_OPERATION(TIMER_MOD),
      LOTUS_KERNEL_OPERATION(TIMER_DELETE),
      LOTUS_KERNEL_OPERATION(TIMER_SHUTDOWN),
      LOTUS_KERNEL_OPERATION(MEMORY_BARRIER),
      LOTUS_KERNEL_OPERATION(ATOMIC_READ),
      LOTUS_KERNEL_OPERATION(ATOMIC_WRITE),
      LOTUS_KERNEL_OPERATION(ATOMIC_RMW),
      LOTUS_KERNEL_OPERATION(KTHREAD_CREATE),
      LOTUS_KERNEL_OPERATION(KTHREAD_START),
      LOTUS_KERNEL_OPERATION(KTHREAD_RUN),
      LOTUS_KERNEL_OPERATION(KTHREAD_STOP),
      LOTUS_KERNEL_OPERATION(KTHREAD_SHOULD_STOP),
      LOTUS_KERNEL_OPERATION(WORKqueue),
      LOTUS_KERNEL_OPERATION(WORKqueue_CREATE),
      LOTUS_KERNEL_OPERATION(WORKqueue_SUBMIT),
      LOTUS_KERNEL_OPERATION(WORKqueue_FLUSH),
      LOTUS_KERNEL_OPERATION(WORKqueue_CANCEL),
      LOTUS_KERNEL_OPERATION(WORKqueue_DESTROY),
      LOTUS_KERNEL_OPERATION(SOFTIRQ_REGISTER),
      LOTUS_KERNEL_OPERATION(SOFTIRQ_RAISE),
      LOTUS_KERNEL_OPERATION(TASKLET_SETUP),
      LOTUS_KERNEL_OPERATION(TASKLET_SCHEDULE),
      LOTUS_KERNEL_OPERATION(TASKLET_KILL),
      LOTUS_KERNEL_OPERATION(NAPI_REGISTER),
      LOTUS_KERNEL_OPERATION(NAPI_SCHEDULE),
      LOTUS_KERNEL_OPERATION(NAPI_DISABLE),
      LOTUS_KERNEL_OPERATION(IRQ_REQUEST),
      LOTUS_KERNEL_OPERATION(IRQ_FREE),
      LOTUS_KERNEL_OPERATION(IRQ_ENABLE),
      LOTUS_KERNEL_OPERATION(IRQ_DISABLE),
      LOTUS_KERNEL_OPERATION(IRQ_LINE_ENABLE),
      LOTUS_KERNEL_OPERATION(IRQ_LINE_DISABLE),
      LOTUS_KERNEL_OPERATION(BH_ENABLE),
      LOTUS_KERNEL_OPERATION(BH_DISABLE),
      LOTUS_KERNEL_OPERATION(PREEMPT_ENABLE),
      LOTUS_KERNEL_OPERATION(PREEMPT_DISABLE),
      LOTUS_KERNEL_OPERATION(KMALLOC),
      LOTUS_KERNEL_OPERATION(VMALLOC),
      LOTUS_KERNEL_OPERATION(ALLOC_PAGES),
      LOTUS_KERNEL_OPERATION(MEMORY_FREE),
      LOTUS_KERNEL_OPERATION(UNKNOWN_CALL),
#undef LOTUS_KERNEL_OPERATION
  };
  return parseEnum(value, operations, result);
}

bool parseBool(StringRef value, bool &result) {
  if (value.equals_insensitive("true") || value == "1" ||
      value.equals_insensitive("yes")) {
    result = true;
    return true;
  }
  if (value.equals_insensitive("false") || value == "0" ||
      value.equals_insensitive("no")) {
    result = false;
    return true;
  }
  return false;
}

bool parseOperand(StringRef value, int &result) {
  if (value == "result") {
    result = LinuxKernelAPISemantics::ReturnValue;
    return true;
  }
  long long parsed = 0;
  if (value.getAsInteger(10, parsed) || parsed < -1 || parsed > 1024) {
    return false;
  }
  result = static_cast<int>(parsed);
  return true;
}

std::string trimComment(StringRef line) {
  return line.take_front(line.find('#')).trim().str();
}

} // namespace

void LinuxKernelSemanticRegistry::add(LinuxKernelAPISemantics semantics) {
  if (semantics.match == LinuxKernelAPISemantics::MatchKind::EXACT) {
    exact_[semantics.pattern] = std::move(semantics);
    return;
  }
  prefixes_.erase(std::remove_if(prefixes_.begin(), prefixes_.end(),
                                 [&](const LinuxKernelAPISemantics &existing) {
                                   return existing.pattern == semantics.pattern;
                                 }),
                  prefixes_.end());
  prefixes_.push_back(std::move(semantics));
  llvm::sort(prefixes_, [](const LinuxKernelAPISemantics &lhs,
                           const LinuxKernelAPISemantics &rhs) {
    return lhs.pattern.size() > rhs.pattern.size();
  });
}

bool LinuxKernelSemanticRegistry::loadFile(StringRef path) {
  std::ifstream input(path.str());
  if (!input.is_open()) {
    errors_.push_back("cannot open Linux kernel API spec: " + path.str());
    return false;
  }

  std::string line;
  unsigned line_number = 0;
  bool valid = true;
  std::vector<LinuxKernelAPISemantics> parsed_semantics;
  while (std::getline(input, line)) {
    ++line_number;
    line = trimComment(line);
    if (line.empty()) {
      continue;
    }
    std::istringstream tokens(line);
    LinuxKernelAPISemantics semantics;
    std::string operation;
    if (!(tokens >> semantics.pattern >> operation) ||
        !parseOperation(operation, semantics.operation)) {
      errors_.push_back(path.str() + ":" + std::to_string(line_number) +
                        ": invalid symbol or operation");
      valid = false;
      continue;
    }
    semantics.source = path.str() + ":" + std::to_string(line_number);

    std::string attribute;
    bool row_valid = true;
    while (tokens >> attribute) {
      StringRef item(attribute);
      auto split = item.split('=');
      if (split.second.empty()) {
        row_valid = false;
        break;
      }
      const StringRef key = split.first;
      const StringRef value = split.second;
      if (key == "match") {
        if (value == "exact") {
          semantics.match = LinuxKernelAPISemantics::MatchKind::EXACT;
        } else if (value == "prefix") {
          semantics.match = LinuxKernelAPISemantics::MatchKind::PREFIX;
        } else {
          row_valid = false;
        }
      } else if (key == "lock") {
        static const std::unordered_map<std::string, LockKind> locks = {
            {"spin", LockKind::SPINLOCK},
            {"raw-spin", LockKind::SPINLOCK},
            {"mutex", LockKind::MUTEX},
            {"rwlock", LockKind::RWLOCK},
            {"rwsem", LockKind::RW_SEMAPHORE},
            {"semaphore", LockKind::SEMAPHORE},
            {"seqlock", LockKind::SEQCOUNT}};
        row_valid = parseEnum(value, locks, semantics.lock_kind);
      } else if (key == "mode") {
        static const std::unordered_map<std::string, LockMode> modes = {
            {"shared", LockMode::SHARED},
            {"exclusive", LockMode::EXCLUSIVE},
            {"unknown", LockMode::UNKNOWN}};
        row_valid = parseEnum(value, modes, semantics.lock_mode);
      } else if (key == "success") {
        static const std::unordered_map<std::string, ConditionalSuccess>
            success = {{"unconditional", ConditionalSuccess::UNCONDITIONAL},
                       {"zero", ConditionalSuccess::ZERO},
                       {"nonzero", ConditionalSuccess::NONZERO}};
        row_valid = parseEnum(value, success, semantics.success);
      } else if (key == "context") {
        static const std::unordered_map<std::string, AsyncContextKind>
            contexts = {{"task", AsyncContextKind::TASK},
                        {"kthread", AsyncContextKind::KTHREAD},
                        {"workqueue", AsyncContextKind::WORKQUEUE},
                        {"timer", AsyncContextKind::TIMER_SOFTIRQ},
                        {"hardirq", AsyncContextKind::HARDIRQ},
                        {"threaded-irq", AsyncContextKind::THREADED_IRQ},
                        {"rcu", AsyncContextKind::RCU_CALLBACK},
                        {"softirq", AsyncContextKind::SOFTIRQ},
                        {"tasklet", AsyncContextKind::TASKLET},
                        {"napi", AsyncContextKind::NAPI},
                        {"nmi", AsyncContextKind::NMI}};
        row_valid = parseEnum(value, contexts, semantics.async_context);
      } else if (key == "secondary-context") {
        static const std::unordered_map<std::string, AsyncContextKind>
            contexts = {{"task", AsyncContextKind::TASK},
                        {"kthread", AsyncContextKind::KTHREAD},
                        {"workqueue", AsyncContextKind::WORKQUEUE},
                        {"timer", AsyncContextKind::TIMER_SOFTIRQ},
                        {"hardirq", AsyncContextKind::HARDIRQ},
                        {"threaded-irq", AsyncContextKind::THREADED_IRQ},
                        {"rcu", AsyncContextKind::RCU_CALLBACK},
                        {"softirq", AsyncContextKind::SOFTIRQ},
                        {"tasklet", AsyncContextKind::TASKLET},
                        {"napi", AsyncContextKind::NAPI},
                        {"nmi", AsyncContextKind::NMI}};
        row_valid =
            parseEnum(value, contexts, semantics.secondary_async_context);
      } else if (key == "rcu-flavor") {
        static const std::unordered_map<std::string, RCUFlavor> flavors = {
            {"classic", RCUFlavor::CLASSIC},
            {"bh", RCUFlavor::BH},
            {"sched", RCUFlavor::SCHED},
            {"srcu", RCUFlavor::SRCU},
            {"tasks", RCUFlavor::TASKS},
            {"tasks-trace", RCUFlavor::TASKS_TRACE}};
        row_valid = parseEnum(value, flavors, semantics.rcu_flavor);
      } else if (key == "completion") {
        static const std::unordered_map<std::string, CompletionSignalKind>
            signals = {{"one", CompletionSignalKind::ONE},
                       {"all", CompletionSignalKind::ALL},
                       {"unknown", CompletionSignalKind::UNKNOWN}};
        row_valid = parseEnum(value, signals, semantics.completion_signal);
      } else if (key == "order") {
        static const std::unordered_map<std::string, KernelMemoryOrder> orders =
            {{"none", KernelMemoryOrder::NONE},
             {"relaxed", KernelMemoryOrder::RELAXED},
             {"acquire", KernelMemoryOrder::ACQUIRE},
             {"release", KernelMemoryOrder::RELEASE},
             {"acq-rel", KernelMemoryOrder::ACQ_REL},
             {"full", KernelMemoryOrder::FULL},
             {"compiler", KernelMemoryOrder::COMPILER},
             {"unknown", KernelMemoryOrder::UNKNOWN}};
        row_valid = parseEnum(value, orders, semantics.memory_order);
      } else if (key == "object") {
        row_valid = parseOperand(value, semantics.object_arg);
      } else if (key == "callback") {
        row_valid = parseOperand(value, semantics.callback_arg);
      } else if (key == "secondary-callback") {
        row_valid = parseOperand(value, semantics.secondary_callback_arg);
      } else if (key == "domain") {
        row_valid = parseOperand(value, semantics.domain_arg);
      } else if (key == "condition") {
        row_valid = parseOperand(value, semantics.condition_arg);
      } else if (key == "flags") {
        row_valid = parseOperand(value, semantics.flags_arg);
      } else if (key == "value") {
        row_valid = parseOperand(value, semantics.value_arg);
      } else if (key == "size") {
        row_valid = parseOperand(value, semantics.size_arg);
      } else if (key == "expires") {
        row_valid = parseOperand(value, semantics.expires_arg);
      } else if (key == "subclass") {
        row_valid = parseOperand(value, semantics.subclass_arg);
      } else if (key == "synchronous") {
        row_valid = parseBool(value, semantics.synchronous);
      } else if (key == "serializes-domain") {
        row_valid = parseBool(value, semantics.serializes_domain);
      } else if (key == "raw-lock") {
        row_valid = parseBool(value, semantics.raw_lock);
      } else if (key == "nested-lock") {
        row_valid = parseBool(value, semantics.nested_lock);
      } else if (key == "interruptible") {
        row_valid = parseBool(value, semantics.interruptible);
      } else if (key == "timeout") {
        row_valid = parseBool(value, semantics.timeout);
      } else if (key == "wake-all") {
        row_valid = parseBool(value, semantics.wake_all);
      } else if (key == "wake-exclusive") {
        row_valid = parseBool(value, semantics.wake_exclusive);
      } else if (key == "deferred-reclamation") {
        row_valid = parseBool(value, semantics.deferred_reclamation);
      } else if (key == "returns-retired-pointer") {
        row_valid = parseBool(value, semantics.returns_retired_pointer);
      } else if (key == "requires-rcu-section") {
        row_valid = parseBool(value, semantics.requires_rcu_section);
      } else if (key == "managed-allocation") {
        row_valid = parseBool(value, semantics.managed_allocation);
      } else if (key == "may-sleep") {
        row_valid = parseBool(value, semantics.may_sleep);
      } else if (key == "may-spawn") {
        row_valid = parseBool(value, semantics.may_spawn);
      } else if (key == "may-access-shared") {
        row_valid = parseBool(value, semantics.may_access_shared_memory);
      } else if (key == "saves-irq-state") {
        row_valid = parseBool(value, semantics.saves_irq_state);
      } else if (key == "restores-irq-state") {
        row_valid = parseBool(value, semantics.restores_irq_state);
      } else if (key == "disables-local-irq") {
        row_valid = parseBool(value, semantics.disables_local_irq);
      } else if (key == "enables-local-irq") {
        row_valid = parseBool(value, semantics.enables_local_irq);
      } else if (key == "disables-bh") {
        row_valid = parseBool(value, semantics.disables_bh);
      } else if (key == "enables-bh") {
        row_valid = parseBool(value, semantics.enables_bh);
      } else if (key == "disables-preemption") {
        row_valid = parseBool(value, semantics.disables_preemption);
      } else if (key == "enables-preemption") {
        row_valid = parseBool(value, semantics.enables_preemption);
      } else if (key == "preemption-effect-non-rt") {
        row_valid = parseBool(value, semantics.preemption_effect_non_rt);
      } else {
        row_valid = false;
      }
      if (!row_valid) {
        break;
      }
    }
    if (!row_valid) {
      errors_.push_back(path.str() + ":" + std::to_string(line_number) +
                        ": invalid API semantic attribute");
      valid = false;
      continue;
    }
    parsed_semantics.push_back(std::move(semantics));
  }
  if (!valid) {
    return false;
  }
  for (LinuxKernelAPISemantics &semantics : parsed_semantics) {
    add(std::move(semantics));
  }
  loaded_files_.push_back(path.str());
  return true;
}

void LinuxKernelSemanticRegistry::load(const LinuxKernelConfig &config) {
  exact_.clear();
  prefixes_.clear();
  loaded_files_.clear();
  errors_.clear();

  if (config.load_default_api_specs) {
    std::vector<std::string> directories;
    if (const char *override_dir = std::getenv("LOTUS_CONFIG_DIR")) {
      if (*override_dir != '\0') {
        directories.emplace_back(override_dir);
      }
    }
    const bool has_override = !directories.empty();
#ifdef LOTUS_SOURCE_CONFIG_DIR
    if (!has_override) {
      directories.emplace_back(LOTUS_SOURCE_CONFIG_DIR);
    }
#endif
#ifdef LOTUS_INSTALL_CONFIG_DIR
    if (!has_override) {
      directories.emplace_back(LOTUS_INSTALL_CONFIG_DIR);
    }
#endif
    if (!has_override) {
      directories.emplace_back("config");
      directories.emplace_back("../config");
    }

    bool loaded_default = false;
    for (const std::string &directory : directories) {
      const std::string path = directory + "/linux_kernel_api.spec";
      std::ifstream probe(path);
      if (!probe.is_open()) {
        continue;
      }
      probe.close();
      loaded_default = loadFile(path);
      break;
    }
    if (!loaded_default && config.require_api_specs) {
      errors_.push_back("default Linux kernel API spec was not found");
    }
  }

  for (const std::string &path : config.api_spec_paths) {
    loadFile(path);
  }

  if (config.require_api_specs && loaded_files_.empty()) {
    errors_.push_back("no Linux kernel API semantic spec was loaded");
  }

  if (config.require_api_specs && !errors_.empty()) {
    for (const std::string &error : errors_) {
      errs() << "Linux kernel API spec error: " << error << '\n';
    }
    // Required semantic inputs are atomic: a partially loaded registry could
    // silently assign the wrong argument layout or execution context.
    exact_.clear();
    prefixes_.clear();
  }
}

const LinuxKernelAPISemantics *
LinuxKernelSemanticRegistry::lookup(StringRef symbol) const {
  auto exact = exact_.find(symbol);
  if (exact != exact_.end()) {
    return &exact->second;
  }
  for (const LinuxKernelAPISemantics &semantics : prefixes_) {
    if (symbol.startswith(semantics.pattern)) {
      return &semantics;
    }
  }
  return nullptr;
}

} // namespace kernel
