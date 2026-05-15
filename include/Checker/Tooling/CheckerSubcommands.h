#pragma once

#include <llvm/Support/CommandLine.h>

namespace lotus::checker::tooling {

inline llvm::cl::SubCommand &genericSubCommand() {
  static llvm::cl::SubCommand sub("generic",
                                  "Run declarative and registry-backed checks");
  return sub;
}

inline llvm::cl::SubCommand &kintSubCommand() {
  static llvm::cl::SubCommand sub("kint", "Run the KINT integer checker");
  return sub;
}

inline llvm::cl::SubCommand &taintSubCommand() {
  static llvm::cl::SubCommand sub("taint",
                                  "Run the IFDS-based taint analysis");
  return sub;
}

inline llvm::cl::SubCommand &concurrencySubCommand() {
  static llvm::cl::SubCommand sub("concur",
                                  "Run the concurrency checker suite");
  return sub;
}

inline llvm::cl::SubCommand &pulseSubCommand() {
  static llvm::cl::SubCommand sub("pulse", "Run the Pulse checker");
  return sub;
}

inline llvm::cl::SubCommand &fitxSubCommand() {
  static llvm::cl::SubCommand sub("fitx", "Run the FiTx checker suite");
  return sub;
}

inline llvm::cl::SubCommand &saberSubCommand() {
  static llvm::cl::SubCommand sub("saber", "Run the Saber checker");
  return sub;
}

inline llvm::cl::SubCommand &aeSubCommand() {
  static llvm::cl::SubCommand sub("ae",
                                  "Run the abstract-execution checker");
  return sub;
}

inline llvm::cl::SubCommand &symexSubCommand() {
  static llvm::cl::SubCommand sub("symex",
                                  "Run the symbolic-execution checker");
  return sub;
}

} // namespace lotus::checker::tooling
