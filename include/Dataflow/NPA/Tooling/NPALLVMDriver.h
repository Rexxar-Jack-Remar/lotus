#pragma once

#include "llvm/Support/raw_ostream.h"

#include <string>

int runNpaLLVM(const std::string &input_filename, llvm::raw_ostream &os,
               const std::string &analysis, const std::string &solver,
               const std::string &linear_solver,
               const std::string &newton_round, bool print_block_results,
               bool print_newton_profile, const char *argv0);
