/** @file CheckerDiagnostic.h @brief Diagnostic types and utilities for checker bug reports. */
#pragma once

#include "Checker/Core/CheckerTypes.h"
#include "Checker/Report/BugReport.h"

#include <llvm/IR/Value.h>

#include <map>
#include <string>
#include <vector>

namespace lotus::checker {

struct CheckerTraceStep {
  const llvm::Value *value = nullptr;
  std::string message;
  int trace_level = 0;
};

struct CheckerDiagnostic {
  std::string checker_id;
  std::string bug_type;
  Severity severity = Severity::Medium;
  const llvm::Value *primary_value = nullptr;
  std::string message;
  std::string suggestion;
  int confidence = 80;
  std::map<std::string, std::string> metadata;
  std::vector<CheckerTraceStep> trace;

  BugReport *toBugReport(int bug_type_id) const;
};

} // namespace lotus::checker
