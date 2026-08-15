#include "JsonFrontend.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

namespace {

void usage(llvm::raw_ostream &output) {
  output << "usage:\n"
            "  lotus-datalog run <program.json|-> [options]\n"
            "  lotus-datalog validate <program.json|->\n"
            "  lotus-datalog schema\n\n"
            "options:\n"
            "  --workers N\n"
            "  --grain-size N\n"
            "  --pretty\n"
            "  --trace-scc\n"
            "  --trace-rule\n"
            "  --trace-delta\n";
}

std::size_t parseSize(const char *value, const char *option) {
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (!end || *end != '\0' || parsed == 0)
    throw std::invalid_argument(std::string(option) + " requires a positive integer");
  return static_cast<std::size_t>(parsed);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(llvm::errs());
    return 2;
  }

  const std::string command = argv[1];
  if (command == "schema") {
    lotus::datalog::cli::printSchema(llvm::outs());
    return 0;
  }
  if (command != "run" && command != "validate") {
    usage(llvm::errs());
    return 2;
  }
  if (argc < 3) {
    llvm::errs() << "missing JSON program path\n";
    return 2;
  }

  lotus::datalog::cli::RunOptions options;
  options.validate_only = command == "validate";
  options.execution.trace_stream = &std::cerr;
  for (int index = 3; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--workers" && index + 1 < argc) {
      options.execution.worker_count =
          parseSize(argv[++index], "--workers");
    } else if (argument == "--grain-size" && index + 1 < argc) {
      options.execution.parallel_grain_size =
          parseSize(argv[++index], "--grain-size");
    } else if (argument == "--pretty") {
      options.pretty = true;
    } else if (argument == "--trace-scc") {
      options.execution.trace_scc = true;
    } else if (argument == "--trace-rule") {
      options.execution.trace_rule = true;
    } else if (argument == "--trace-delta") {
      options.execution.trace_delta = true;
    } else {
      llvm::errs() << "unknown option: " << argument << '\n';
      return 2;
    }
  }

  auto input = llvm::MemoryBuffer::getFileOrSTDIN(argv[2]);
  if (!input) {
    llvm::errs() << "cannot read '" << argv[2]
                 << "': " << input.getError().message() << '\n';
    return 2;
  }

  try {
    lotus::datalog::cli::executeJson(input.get()->getBuffer(), options,
                                     llvm::outs());
  } catch (const std::exception &error) {
    llvm::errs() << "lotus-datalog: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
