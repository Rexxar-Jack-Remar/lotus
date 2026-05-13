#include "Checker/Core/CheckerDriver.h"

#include "Checker/Report/BugReportMgr.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Error.h>

#include <map>
#include <queue>
#include <set>

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
  std::queue<const Value *> worklist;

  auto enqueue_tainted = [&](const Value *value) {
    if (value && tainted_values.insert(value).second) {
      worklist.push(value);
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

  while (!worklist.empty()) {
    const Value *value = worklist.front();
    worklist.pop();

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
          tainted_memory.insert(store->getPointerOperand());
        }
        continue;
      }
      if (const auto *load = dyn_cast<LoadInst>(user)) {
        if (tainted_memory.contains(load->getPointerOperand())) {
          enqueue_tainted(load);
        }
      }
    }
  }

  return diagnostics;
}

static std::vector<CheckerDiagnostic>
runApiProtocol(const CheckerSpec &spec, CheckerContext &context) {
  enum class ResourceState { Unknown, Acquired, Released };

  std::vector<CheckerDiagnostic> diagnostics;
  std::map<const Value *, ResourceState> states;
  std::set<const Value *> ever_acquired;

  auto emit = [&](const CallBase *call, StringRef message, StringRef kind,
                  const Value *tracked) {
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

      if (anyNameMatches(name, spec.api_protocol.acquire)) {
        const Value *resource = call;
        ResourceState &state = states[resource];
        if (state == ResourceState::Acquired &&
            spec.api_protocol.report_double_acquire) {
          emit(call, spec.message + " (double acquire)", "double-acquire",
               resource);
        }
        state = ResourceState::Acquired;
        ever_acquired.insert(resource);
        continue;
      }

      if (call->arg_empty()) {
        continue;
      }

      const Value *resource = call->getArgOperand(0);
      ResourceState &state = states[resource];

      if (anyNameMatches(name, spec.api_protocol.use)) {
        if (state == ResourceState::Unknown &&
            spec.api_protocol.report_use_before_acquire) {
          emit(call, spec.message + " (use before acquire)",
               "use-before-acquire", resource);
        } else if (state == ResourceState::Released &&
                   spec.api_protocol.report_use_after_release) {
          emit(call, spec.message + " (use after release)",
               "use-after-release", resource);
        }
        continue;
      }

      if (anyNameMatches(name, spec.api_protocol.release)) {
        if (state == ResourceState::Released &&
            spec.api_protocol.report_use_after_release) {
          emit(call, spec.message + " (double release)", "double-release",
               resource);
        }
        state = ResourceState::Released;
      }
    }
  }

  if (spec.api_protocol.report_leak) {
    for (const auto *resource : ever_acquired) {
      const auto it = states.find(resource);
      if (it != states.end() && it->second == ResourceState::Acquired) {
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
