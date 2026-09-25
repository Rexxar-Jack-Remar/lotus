/**
 * @file ConstantPropagationAnalysis.h
 * @brief Header for constant propagation analysis using WPDS-based dataflow
 * engine
 *
 * Author: rainoftime
 */

#pragma once

#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/WPDS/Backend.h"

#include <memory>

#include <llvm/IR/Module.h>

/**
 * @brief Runs constant propagation analysis and returns detailed results
 *
 * @param module The LLVM module to analyze
 * @return Analysis result containing IN/OUT/GEN/KILL sets for each instruction
 */
std::unique_ptr<mono::DataFlowResult>
runConstantPropagationAnalysis(llvm::Module &module);
std::unique_ptr<mono::DataFlowResult>
runConstantPropagationAnalysis(llvm::Module &module,
                               wpds::WPDSBackendOptions options);
std::unique_ptr<mono::DataFlowResult> runConstantPropagationAnalysis(
    llvm::Module &module, wpds::WPDSBackendOptions options,
    wpds::WPDSBackendStatistics *statistics, std::string *error);

/**
 * @brief Demo function showing how to use the constant propagation analysis
 *
 * @param module The LLVM module to analyze
 */
void demoConstantPropagationAnalysis(llvm::Module &module);

