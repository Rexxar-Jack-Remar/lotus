#include "Checker/Core/CheckerSpecLoader.h"

#include "Checker/Core/CheckerValidator.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/YAMLTraits.h>

#include <filesystem>
#include <optional>
#include <unordered_set>

using namespace llvm;

namespace fs = std::filesystem;

namespace lotus::checker {

struct YamlMetadata {
  std::string id;
  std::string title;
  std::string category;
  std::string summary;
  std::string severity;
  std::vector<std::string> languages;
  std::vector<std::string> tags;
  bool default_enabled = true;
};

struct YamlSpec {
  std::string engine;
  std::string rule_kind;
  YamlMetadata metadata;
  std::vector<std::string> capabilities;
  std::string message;
  std::string suggestion;
  int confidence = 80;
  std::vector<std::string> functions;
  std::vector<std::string> sources;
  std::vector<std::string> sinks;
  std::vector<std::string> sanitizers;
  std::vector<std::string> acquire;
  std::vector<std::string> use;
  std::vector<std::string> release;
  bool report_leak = true;
  bool report_use_before_acquire = true;
  bool report_use_after_release = true;
  bool report_double_acquire = true;
};

namespace {

EngineKind parseEngineKind(StringRef value) {
  if (value == "declarative") {
    return EngineKind::Declarative;
  }
  if (value == "ae") {
    return EngineKind::AE;
  }
  if (value == "saber") {
    return EngineKind::Saber;
  }
  if (value == "pulse") {
    return EngineKind::Pulse;
  }
  if (value == "kint") {
    return EngineKind::KINT;
  }
  if (value == "taint") {
    return EngineKind::Taint;
  }
  if (value == "fitx") {
    return EngineKind::FiTx;
  }
  if (value == "concurrency") {
    return EngineKind::Concurrency;
  }
  return EngineKind::SymExec;
}

Severity parseSeverity(StringRef value) {
  if (value == "low") {
    return Severity::Low;
  }
  if (value == "high") {
    return Severity::High;
  }
  if (value == "critical") {
    return Severity::Critical;
  }
  return Severity::Medium;
}

RuleKind parseRuleKind(StringRef value) {
  if (value == "forbidden_call") {
    return RuleKind::ForbiddenCall;
  }
  if (value == "source_sink") {
    return RuleKind::SourceSink;
  }
  if (value == "api_protocol") {
    return RuleKind::ApiProtocol;
  }
  return RuleKind::Native;
}

std::optional<CheckerCapability> parseCapability(StringRef value) {
  if (value == "debug-info") {
    return CheckerCapability::DebugInfo;
  }
  if (value == "direct-calls") {
    return CheckerCapability::DirectCalls;
  }
  if (value == "use-def") {
    return CheckerCapability::UseDef;
  }
  if (value == "simple-memory") {
    return CheckerCapability::SimpleMemory;
  }
  if (value == "interprocedural-flow") {
    return CheckerCapability::InterproceduralFlow;
  }
  if (value == "icfg") {
    return CheckerCapability::ICFG;
  }
  if (value == "svfg") {
    return CheckerCapability::SVFG;
  }
  if (value == "pta") {
    return CheckerCapability::PTA;
  }
  if (value == "mhp") {
    return CheckerCapability::MHP;
  }
  if (value == "smt") {
    return CheckerCapability::SMT;
  }
  return std::nullopt;
}

} // namespace
} // namespace lotus::checker

namespace llvm::yaml {

template <> struct MappingTraits<lotus::checker::YamlMetadata> {
  static void mapping(IO &io, lotus::checker::YamlMetadata &metadata) {
    io.mapRequired("id", metadata.id);
    io.mapRequired("title", metadata.title);
    io.mapRequired("category", metadata.category);
    io.mapOptional("summary", metadata.summary);
    io.mapOptional("severity", metadata.severity, std::string("medium"));
    io.mapOptional("languages", metadata.languages);
    io.mapOptional("tags", metadata.tags);
    io.mapOptional("default_enabled", metadata.default_enabled, true);
  }
};

template <> struct MappingTraits<lotus::checker::YamlSpec> {
  static void mapping(IO &io, lotus::checker::YamlSpec &spec) {
    io.mapRequired("engine", spec.engine);
    io.mapRequired("rule_kind", spec.rule_kind);
    io.mapRequired("metadata", spec.metadata);
    io.mapOptional("capabilities", spec.capabilities);
    io.mapRequired("message", spec.message);
    io.mapOptional("suggestion", spec.suggestion);
    io.mapOptional("confidence", spec.confidence, 80);
    io.mapOptional("functions", spec.functions);
    io.mapOptional("sources", spec.sources);
    io.mapOptional("sinks", spec.sinks);
    io.mapOptional("sanitizers", spec.sanitizers);
    io.mapOptional("acquire", spec.acquire);
    io.mapOptional("use", spec.use);
    io.mapOptional("release", spec.release);
    io.mapOptional("report_leak", spec.report_leak, true);
    io.mapOptional("report_use_before_acquire",
                   spec.report_use_before_acquire, true);
    io.mapOptional("report_use_after_release",
                   spec.report_use_after_release, true);
    io.mapOptional("report_double_acquire", spec.report_double_acquire, true);
  }
};

} // namespace llvm::yaml

namespace lotus::checker {

Expected<CheckerSpec> CheckerSpecLoader::loadFromBuffer(StringRef yaml,
                                                        StringRef source_name) const {
  yaml::Input input(yaml);
  YamlSpec parsed;
  input >> parsed;
  if (std::error_code error = input.error()) {
    return createStringError(error, "failed to parse YAML spec %s",
                             source_name.data());
  }

  CheckerSpec spec;
  spec.metadata.id = parsed.metadata.id;
  spec.metadata.title = parsed.metadata.title;
  spec.metadata.category = parsed.metadata.category;
  spec.metadata.summary = parsed.metadata.summary;
  spec.metadata.severity = parseSeverity(parsed.metadata.severity);
  spec.metadata.engine = parseEngineKind(parsed.engine);
  spec.metadata.languages = parsed.metadata.languages;
  spec.metadata.tags = parsed.metadata.tags;
  spec.metadata.default_enabled = parsed.metadata.default_enabled;
  spec.rule_kind = parseRuleKind(parsed.rule_kind);
  spec.message = parsed.message;
  spec.suggestion = parsed.suggestion;
  spec.confidence = parsed.confidence;
  spec.forbidden_call.functions = parsed.functions;
  spec.source_sink.sources = parsed.sources;
  spec.source_sink.sinks = parsed.sinks;
  spec.source_sink.sanitizers = parsed.sanitizers;
  spec.api_protocol.acquire = parsed.acquire;
  spec.api_protocol.use = parsed.use;
  spec.api_protocol.release = parsed.release;
  spec.api_protocol.report_leak = parsed.report_leak;
  spec.api_protocol.report_use_before_acquire =
      parsed.report_use_before_acquire;
  spec.api_protocol.report_use_after_release =
      parsed.report_use_after_release;
  spec.api_protocol.report_double_acquire = parsed.report_double_acquire;

  for (const auto &capability_name : parsed.capabilities) {
    auto capability = parseCapability(capability_name);
    if (!capability.has_value()) {
      return createStringError(inconvertibleErrorCode(),
                               "unknown capability '%s' in spec %s",
                               capability_name.c_str(), source_name.data());
    }
    spec.capabilities.push_back(*capability);
  }

  if (Error error = CheckerValidator::validate(spec)) {
    return std::move(error);
  }

  return spec;
}

Expected<std::vector<CheckerSpec>>
CheckerSpecLoader::loadFromDirectory(StringRef directory) const {
  std::vector<CheckerSpec> specs;
  std::unordered_set<std::string> ids;
  std::error_code fs_error;
  fs::path root(directory.str());

  if (!fs::exists(root, fs_error) || !fs::is_directory(root, fs_error)) {
    return createStringError(inconvertibleErrorCode(),
                             "spec directory does not exist: %s",
                             directory.data());
  }

  for (const auto &entry : fs::recursive_directory_iterator(root, fs_error)) {
    if (fs_error) {
      return createStringError(fs_error, "failed while walking spec directory");
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".yml") {
      continue;
    }

    auto buffer = MemoryBuffer::getFile(entry.path().string());
    if (!buffer) {
      return createStringError(buffer.getError(), "failed to open spec file");
    }

    auto spec_or = loadFromBuffer(buffer.get()->getBuffer(),
                                  entry.path().string());
    if (!spec_or) {
      return spec_or.takeError();
    }
    if (Error error = CheckerValidator::validate(*spec_or, ids)) {
      return std::move(error);
    }
    ids.insert(spec_or->metadata.id);
    specs.push_back(std::move(*spec_or));
  }

  return specs;
}

} // namespace lotus::checker
