#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace concurrency::cuda::ptx {

struct SourceSpan {
  size_t line = 0;
  size_t column = 0;
};

enum class Severity { Error, Warning, Note };

enum class ReportStatus { Safe, Unsafe, Unsupported, InvalidInput, Unknown };

struct Diagnostic {
  std::string code;
  Severity severity = Severity::Error;
  std::string message;
  std::optional<SourceSpan> span;
  std::optional<uint32_t> thread;
};

struct MemoryFootprint {
  std::string space;
  std::string base;
  int64_t offset = 0;
  uint32_t width = 0;
  uint32_t thread = 0;
  bool is_write = false;
};

struct AnalysisReport {
  ReportStatus status = ReportStatus::Safe;
  std::vector<Diagnostic> diagnostics;
  std::vector<MemoryFootprint> footprints;

  void push(Diagnostic diagnostic);
  void refreshStatus();
  std::string toText() const;
  std::string toJson() const;
};

struct KernelConfig {
  std::optional<std::string> entry;
  std::array<uint32_t, 3> block_dim{{1, 1, 1}};
  std::array<uint32_t, 3> block_idx{{0, 0, 0}};
  std::unordered_map<std::string, uint64_t> pointer_extents;

  uint32_t threadCount() const;

  static KernelConfig fromConfigText(const std::string &text);
};

struct DeclAst {
  std::string space;
  std::string name;
  uint64_t bytes = 0;
  SourceSpan span;
};

struct InstructionAst {
  std::optional<std::string> predicate;
  std::string opcode;
  std::vector<std::string> operands;
  std::string text;
  SourceSpan span;
};

enum class StmtKind { Label, Directive, Instruction };

struct StmtAst {
  StmtKind kind = StmtKind::Directive;
  std::string name;
  std::string text;
  SourceSpan span;
  InstructionAst instruction;
};

struct EntryAst {
  std::string name;
  std::vector<std::string> params;
  std::vector<StmtAst> body;
  SourceSpan span;
};

struct ModuleAst {
  std::optional<std::string> version;
  std::vector<EntryAst> entries;
  std::vector<DeclAst> declarations;
};

struct IrInst {
  std::optional<std::string> predicate;
  std::string opcode;
  std::vector<std::string> operands;
  std::string text;
  SourceSpan span;
};

struct KernelIr {
  std::string name;
  std::vector<std::string> params;
  std::vector<IrInst> instructions;
  std::unordered_map<std::string, size_t> labels;
  std::unordered_map<std::string, uint64_t> shared;
};

ModuleAst parseModule(const std::string &source);
KernelIr lowerKernel(const ModuleAst &module,
                     const std::optional<std::string> &entry_name);
AnalysisReport analyzeKernel(const ModuleAst &module,
                             const KernelConfig &config);

const char *toString(Severity severity);
const char *toString(ReportStatus status);

} // namespace concurrency::cuda::ptx
