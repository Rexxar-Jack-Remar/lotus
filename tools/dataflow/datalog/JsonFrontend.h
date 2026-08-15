#pragma once

#include "Dataflow/Datalog/Scheduler.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus::datalog::cli {

struct RunOptions {
  ExecutionOptions execution;
  bool validate_only = false;
  bool pretty = false;
};

void executeJson(llvm::StringRef input, const RunOptions &options,
                 llvm::raw_ostream &output);
void printSchema(llvm::raw_ostream &output);

} // namespace lotus::datalog::cli
