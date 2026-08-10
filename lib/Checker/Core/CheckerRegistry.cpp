#include "Checker/Core/CheckerRegistry.h"

#include <llvm/Support/Error.h>

using namespace llvm;

namespace lotus::checker {

static Error duplicateIdError(StringRef id) {
  return createStringError(inconvertibleErrorCode(),
                           "checker id already registered: %s", id.data());
}

Error CheckerRegistry::registerDeclarative(const CheckerSpec &spec) {
  for (const auto &descriptor : descriptors_) {
    if (descriptor.metadata.id == spec.metadata.id) {
      return duplicateIdError(spec.metadata.id);
    }
  }

  CheckerDescriptor descriptor;
  descriptor.metadata = spec.metadata;
  descriptor.rule_kind = spec.rule_kind;
  descriptor.capabilities = spec.capabilities;
  descriptor.executable = true;
  descriptor.spec = spec;
  descriptors_.push_back(std::move(descriptor));
  return Error::success();
}

Error CheckerRegistry::registerNative(const CheckerMetadata &metadata,
                                      RuleKind rule_kind,
                                      std::vector<CheckerCapability> capabilities,
                                      bool executable) {
  for (const auto &descriptor : descriptors_) {
    if (descriptor.metadata.id == metadata.id) {
      return duplicateIdError(metadata.id);
    }
  }

  CheckerDescriptor descriptor;
  descriptor.metadata = metadata;
  descriptor.rule_kind = rule_kind;
  descriptor.capabilities = std::move(capabilities);
  descriptor.executable = executable;
  descriptors_.push_back(std::move(descriptor));
  return Error::success();
}

Expected<const CheckerDescriptor *> CheckerRegistry::findById(StringRef id) const {
  for (const auto &descriptor : descriptors_) {
    if (descriptor.metadata.id == id) {
      return &descriptor;
    }
  }
  return createStringError(inconvertibleErrorCode(), "unknown checker id: %s",
                           id.data());
}

std::vector<const CheckerDescriptor *> CheckerRegistry::list() const {
  std::vector<const CheckerDescriptor *> result;
  result.reserve(descriptors_.size());
  for (const auto &descriptor : descriptors_) {
    result.push_back(&descriptor);
  }
  return result;
}

std::vector<const CheckerDescriptor *>
CheckerRegistry::select(StringRef category,
                        std::optional<EngineKind> engine) const {
  std::vector<const CheckerDescriptor *> result;
  for (const auto &descriptor : descriptors_) {
    if (!category.empty() && descriptor.metadata.category != category) {
      continue;
    }
    if (engine.has_value() && descriptor.metadata.engine != *engine) {
      continue;
    }
    result.push_back(&descriptor);
  }
  return result;
}

Error registerBuiltinNativeCheckers(CheckerRegistry &registry) {
  auto add_native =
      [&](const char *id, const char *title, const char *category,
          Severity severity, EngineKind engine,
          std::vector<CheckerCapability> capabilities) -> Error {
    CheckerMetadata metadata;
    metadata.id = id;
    metadata.title = title;
    metadata.category = category;
    metadata.summary = title;
    metadata.severity = severity;
    metadata.engine = engine;
    metadata.languages = {"llvm-ir"};
    metadata.default_enabled = false;
    return registry.registerNative(metadata, RuleKind::Native,
                                   std::move(capabilities), false);
  };

  if (Error error = add_native("ae", "Abstract Execution", "memory-safety",
                               Severity::High, EngineKind::AE,
                               {CheckerCapability::ICFG, CheckerCapability::SMT})) {
    return error;
  }
  if (Error error = add_native("saber", "Saber Source-Sink", "memory-safety",
                               Severity::High, EngineKind::Saber,
                               {CheckerCapability::SVFG, CheckerCapability::PTA})) {
    return error;
  }
  if (Error error = add_native("pulse", "Pulse Checker", "memory-safety",
                               Severity::High, EngineKind::Pulse,
                               {CheckerCapability::UseDef, CheckerCapability::SMT})) {
    return error;
  }
  if (Error error = add_native("kint", "KINT", "integer", Severity::High,
                               EngineKind::KINT,
                               {CheckerCapability::SMT, CheckerCapability::ICFG})) {
    return error;
  }
  if (Error error = add_native("taint", "IFDS Taint Analysis", "security",
                               Severity::High, EngineKind::Taint,
                               {CheckerCapability::InterproceduralFlow,
                                CheckerCapability::PTA})) {
    return error;
  }
  if (Error error = add_native("fitx", "FiTx", "api-misuse",
                               Severity::Medium, EngineKind::FiTx,
                               {CheckerCapability::DirectCalls})) {
    return error;
  }
  if (Error error = add_native("concurrency", "Concurrency Checker",
                               "concurrency", Severity::High,
                               EngineKind::Concurrency,
                               {CheckerCapability::MHP, CheckerCapability::PTA})) {
    return error;
  }
  if (Error error = add_native("symex", "Symbolic Execution", "path-sensitive",
                               Severity::High, EngineKind::SymExec,
                               {CheckerCapability::SVFG, CheckerCapability::SMT})) {
    return error;
  }
  return Error::success();
}

} // namespace lotus::checker
