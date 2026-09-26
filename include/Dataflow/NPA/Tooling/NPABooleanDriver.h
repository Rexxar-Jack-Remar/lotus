#pragma once

#include "llvm/Support/raw_ostream.h"

#include <string>

int runNpaBoolean(const std::string &input_filename, llvm::raw_ostream &os,
                  const std::string &solver, const std::string &linear_solver,
                  const std::string &newton_round, const std::string &entry,
                  const std::string &query,
                  bool print_block_results, bool require_tensor,
                  bool prepare_only, bool verify_solvers);
