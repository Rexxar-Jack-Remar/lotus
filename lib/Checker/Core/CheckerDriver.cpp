#include "Checker/Core/CheckerDriver.h"

#include "Checker/Report/BugReportMgr.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Error.h>

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <tuple>

using namespace llvm;

namespace lotus::checker {
namespace {

static std::string calleeName(const CallBase &call) {
  if (const auto *callee = call.getCalledFunction()) {
    return callee->getName().str();
  }
  return {};
}

static bool anyNameMatches(const std::string &name,
                           const std::vector<std::string> &patterns) {
  return std::find(patterns.begin(), patterns.end(), name) != patterns.end();
}

static std::vector<CheckerDiagnostic>
runForbiddenCall(const CheckerSpec &spec, CheckerContext &context) {
  std::vector<CheckerDiagnostic> diagnostics;
  for (auto &function : context.module) {
    for (auto &instruction : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (!call) {
        continue;
      }
      const std::string name = calleeName(*call);
      if (name.empty() || !anyNameMatches(name, spec.forbidden_call.functions)) {
        continue;
      }

      CheckerDiagnostic diagnostic;
      diagnostic.checker_id = spec.metadata.id;
      diagnostic.bug_type = spec.metadata.title;
      diagnostic.severity = spec.metadata.severity;
      diagnostic.primary_value = call;
      diagnostic.message = spec.message;
      diagnostic.suggestion = spec.suggestion;
      diagnostic.confidence = spec.confidence;
      diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
      diagnostic.metadata["callee"] = name;
      diagnostics.push_back(std::move(diagnostic));
    }
  }
  return diagnostics;
}

static std::vector<CheckerDiagnostic>
runSourceSink(const CheckerSpec &spec, CheckerContext &context) {
  std::vector<CheckerDiagnostic> diagnostics;
  SmallPtrSet<const Value *, 32> tainted_values;
  SmallPtrSet<const Value *, 16> sanitized_values;
  SmallPtrSet<const Value *, 16> tainted_memory;
  std::queue<const Value *> value_worklist;
  std::queue<const Value *> memory_worklist;

  auto enqueue_tainted = [&](const Value *value) {
    if (value && tainted_values.insert(value).second) {
      value_worklist.push(value);
    }
  };
  auto enqueue_memory = [&](const Value *pointer) {
    if (pointer && tainted_memory.insert(pointer).second) {
      memory_worklist.push(pointer);
    }
  };

  for (auto &function : context.module) {
    for (auto &instruction : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (!call) {
        continue;
      }
      const std::string name = calleeName(*call);
      if (name.empty()) {
        continue;
      }
      if (anyNameMatches(name, spec.source_sink.sources)) {
        enqueue_tainted(call);
      }
      if (anyNameMatches(name, spec.source_sink.sanitizers)) {
        for (const auto &arg : call->args()) {
          if (tainted_values.contains(arg.get())) {
            sanitized_values.insert(call);
          }
        }
      }
    }
  }

  while (!value_worklist.empty() || !memory_worklist.empty()) {
    if (!memory_worklist.empty()) {
      const Value *pointer = memory_worklist.front();
      memory_worklist.pop();

      for (const User *user : pointer->users()) {
        if (const auto *load = dyn_cast<LoadInst>(user)) {
          if (load->getPointerOperand() == pointer) {
            enqueue_tainted(load);
          }
          continue;
        }
        if (isa<BitCastInst>(user) || isa<GetElementPtrInst>(user)) {
          enqueue_memory(user);
        }
      }
      continue;
    }

    const Value *value = value_worklist.front();
    value_worklist.pop();

    for (const auto *user : value->users()) {
      if (const auto *call = dyn_cast<CallBase>(user)) {
        const std::string name = calleeName(*call);
        if (!name.empty()) {
          if (anyNameMatches(name, spec.source_sink.sanitizers)) {
            sanitized_values.insert(call);
            continue;
          }
          if (anyNameMatches(name, spec.source_sink.sinks) &&
              !sanitized_values.contains(value)) {
            CheckerDiagnostic diagnostic;
            diagnostic.checker_id = spec.metadata.id;
            diagnostic.bug_type = spec.metadata.title;
            diagnostic.severity = spec.metadata.severity;
            diagnostic.primary_value = call;
            diagnostic.message = spec.message;
            diagnostic.suggestion = spec.suggestion;
            diagnostic.confidence = spec.confidence;
            diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
            diagnostic.metadata["source_value"] = value->getName().str();
            diagnostic.metadata["sink"] = name;
            diagnostic.trace.push_back(
                CheckerTraceStep{value, "tainted value reaches sink", 0});
            diagnostic.trace.push_back(
                CheckerTraceStep{call, spec.message, 0});
            diagnostics.push_back(std::move(diagnostic));
          }
        }
      }

      if (isa<BitCastInst>(user) || isa<GetElementPtrInst>(user) ||
          isa<PHINode>(user) || isa<SelectInst>(user)) {
        enqueue_tainted(user);
        continue;
      }
      if (const auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getValueOperand() == value) {
          enqueue_memory(store->getPointerOperand());
        }
        continue;
      }
    }
  }

  return diagnostics;
}

static const ProtocolOperation *
findProtocolOperation(StringRef name,
                      const std::vector<ProtocolOperation> &operations) {
  for (const ProtocolOperation &operation : operations) {
    if (operation.function == name) {
      return &operation;
    }
  }
  return nullptr;
}

static const Value *extractResource(const CallBase &call,
                                    const ProtocolOperation &operation) {
  if (operation.resource_kind == ResourceSelectorKind::Return) {
    return call.getType()->isVoidTy() ? nullptr : &call;
  }
  if (operation.resource_arg >= call.arg_size()) {
    return nullptr;
  }
  return call.getArgOperand(operation.resource_arg);
}

static std::vector<CheckerDiagnostic>
runApiProtocol(const CheckerSpec &spec, CheckerContext &context) {
  enum ResourceState : unsigned {
    Unknown = 0,
    Acquired = 1 << 0,
    Released = 1 << 1,
  };
  using StateMap = std::map<const Value *, unsigned>;

  std::vector<CheckerDiagnostic> diagnostics;
  std::set<std::tuple<const Instruction *, std::string, const Value *>> emitted;

  auto emit = [&](const CallBase *call, StringRef message, StringRef kind,
                  const Value *tracked) {
    const auto key = std::make_tuple(static_cast<const Instruction *>(call),
                                     kind.str(), tracked);
    if (!emitted.insert(key).second) {
      return;
    }
    CheckerDiagnostic diagnostic;
    diagnostic.checker_id = spec.metadata.id;
    diagnostic.bug_type = spec.metadata.title;
    diagnostic.severity = spec.metadata.severity;
    diagnostic.primary_value = call;
    diagnostic.message = message.str();
    diagnostic.suggestion = spec.suggestion;
    diagnostic.confidence = spec.confidence;
    diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
    diagnostic.metadata["protocol_violation"] = kind.str();
    if (tracked && tracked->hasName()) {
      diagnostic.metadata["resource"] = tracked->getName().str();
    }
    diagnostics.push_back(std::move(diagnostic));
  };

  auto mergeStates = [](StateMap &destination, const StateMap &source) {
    bool changed = false;
    for (const auto &[resource, state] : source) {
      unsigned &current = destination[resource];
      const unsigned merged = current | state;
      if (merged != current) {
        current = merged;
        changed = true;
      }
    }
    return changed;
  };

  for (Function &function : context.module) {
    if (function.isDeclaration()) {
      continue;
    }

    std::map<const BasicBlock *, StateMap> in_states;
    std::map<const BasicBlock *, StateMap> out_states;
    std::set<const Value *> acquired_resources;
    std::queue<const BasicBlock *> worklist;
    SmallPtrSet<const BasicBlock *, 16> queued;
    SmallPtrSet<const BasicBlock *, 16> processed;
    worklist.push(&function.getEntryBlock());
    queued.insert(&function.getEntryBlock());

    auto transferBlock = [&](const BasicBlock *block, StateMap &state,
                             bool emit_diagnostics) {
      for (const Instruction &instruction : *block) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        if (!call) {
          continue;
        }
        const std::string name = calleeName(*call);
        if (name.empty()) {
          continue;
        }

        if (const ProtocolOperation *operation =
                findProtocolOperation(name, spec.api_protocol.acquire)) {
          const Value *resource = extractResource(*call, *operation);
          if (!resource) {
            continue;
          }
          const unsigned previous = state[resource];
          if (emit_diagnostics && (previous & Acquired) &&
              spec.api_protocol.report_double_acquire) {
            emit(call, spec.message + " (double acquire)", "double-acquire",
                 resource);
          }
          state[resource] = Acquired;
          acquired_resources.insert(resource);
          continue;
        }

        if (const ProtocolOperation *operation =
                findProtocolOperation(name, spec.api_protocol.use)) {
          const Value *resource = extractResource(*call, *operation);
          if (!resource) {
            continue;
          }
          const unsigned current = state[resource];
          if (emit_diagnostics && current == Unknown &&
              spec.api_protocol.report_use_before_acquire) {
            emit(call, spec.message + " (use before acquire)",
                 "use-before-acquire", resource);
          } else if (emit_diagnostics && (current & Released) &&
                     spec.api_protocol.report_use_after_release) {
            emit(call, spec.message + " (use after release)",
                 "use-after-release", resource);
          }
          continue;
        }

        if (const ProtocolOperation *operation =
                findProtocolOperation(name, spec.api_protocol.release)) {
          const Value *resource = extractResource(*call, *operation);
          if (!resource) {
            continue;
          }
          const unsigned previous = state[resource];
          if (emit_diagnostics && (previous & Released) &&
              spec.api_protocol.report_double_release) {
            emit(call, spec.message + " (double release)", "double-release",
                 resource);
          }
          state[resource] = Released;
        }
      }
    };

    while (!worklist.empty()) {
      const BasicBlock *block = worklist.front();
      worklist.pop();
      queued.erase(block);

      StateMap state = in_states[block];
      transferBlock(block, state, false);

      if (processed.contains(block) && out_states[block] == state) {
        continue;
      }
      processed.insert(block);
      out_states[block] = std::move(state);
      for (const BasicBlock *successor : successors(block)) {
        const bool inputChanged =
            mergeStates(in_states[successor], out_states[block]);
        if ((inputChanged || !processed.contains(successor)) &&
            queued.insert(successor).second) {
          worklist.push(successor);
        }
      }
    }

    for (const BasicBlock &block : function) {
      StateMap state = in_states[&block];
      transferBlock(&block, state, true);
    }

    if (!spec.api_protocol.report_leak) {
      continue;
    }
    for (const BasicBlock &block : function) {
      if (!isa<ReturnInst>(block.getTerminator())) {
        continue;
      }
      const StateMap &state = out_states[&block];
      for (const Value *resource : acquired_resources) {
        const auto it = state.find(resource);
        if (it == state.end() || !(it->second & Acquired)) {
          continue;
        }
        const auto key = std::make_tuple(block.getTerminator(),
                                         std::string("leak"), resource);
        if (!emitted.insert(key).second) {
          continue;
        }
        CheckerDiagnostic diagnostic;
        diagnostic.checker_id = spec.metadata.id;
        diagnostic.bug_type = spec.metadata.title;
        diagnostic.severity = spec.metadata.severity;
        diagnostic.primary_value = resource;
        diagnostic.message = spec.message + " (resource leak)";
        diagnostic.suggestion = spec.suggestion;
        diagnostic.confidence = spec.confidence;
        diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
        diagnostic.metadata["protocol_violation"] = "leak";
        diagnostics.push_back(std::move(diagnostic));
      }
    }
  }

  return diagnostics;
}

static Expected<std::vector<CheckerDiagnostic>>
runDeclarative(const CheckerSpec &spec, CheckerContext &context) {
  switch (spec.rule_kind) {
  case RuleKind::ForbiddenCall:
    return runForbiddenCall(spec, context);
  case RuleKind::SourceSink:
    return runSourceSink(spec, context);
  case RuleKind::ApiProtocol:
    return runApiProtocol(spec, context);
  case RuleKind::Native:
    return createStringError(inconvertibleErrorCode(),
                             "native rule kind is not executable");
  }
  return createStringError(inconvertibleErrorCode(),
                           "unhandled declarative rule kind");
}

} // namespace

Expected<std::vector<CheckerDiagnostic>>
CheckerDriver::run(const std::vector<const CheckerDescriptor *> &selection) const {
  std::vector<CheckerDiagnostic> diagnostics;
  for (const CheckerDescriptor *descriptor : selection) {
    if (descriptor == nullptr) {
      continue;
    }
    if (!descriptor->isDeclarative()) {
      return createStringError(inconvertibleErrorCode(),
                               "checker '%s' is not executable via lotus-check",
                               descriptor->metadata.id.c_str());
    }
    auto diagnostics_or = runDeclarative(*descriptor->spec, context_);
    if (!diagnostics_or) {
      return diagnostics_or.takeError();
    }
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(diagnostics_or->begin()),
                       std::make_move_iterator(diagnostics_or->end()));
  }
  return diagnostics;
}

Error CheckerDriver::emitToReportManager(
    const std::vector<CheckerDiagnostic> &diagnostics) const {
  BugReportMgr &manager = BugReportMgr::get_instance();
  manager.clear_all_reports();

  std::map<std::string, int> bug_type_ids;
  for (const auto &diagnostic : diagnostics) {
    auto it = bug_type_ids.find(diagnostic.bug_type);
    if (it == bug_type_ids.end()) {
      int bug_type_id = manager.register_bug_type(diagnostic.bug_type);
      it = bug_type_ids.emplace(diagnostic.bug_type, bug_type_id).first;
    }
    manager.insert_report(it->second, diagnostic.toBugReport(it->second), true);
  }

  return Error::success();
}

} // namespace lotus::checker
