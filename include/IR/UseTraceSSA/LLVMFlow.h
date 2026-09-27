#ifndef LOTUS_IR_USETRACESSA_LLVMFLOW_H
#define LOTUS_IR_USETRACESSA_LLVMFLOW_H

#include "IR/UseTraceSSA/TraceFlowGraph.h"
#include "IR/UseTraceSSA/LLVMHistory.h"

namespace lotus {
namespace usetracessa {
/// Append non-mutating LLVM scalar histories, including edge-local null facts.
/// This does NOT run alias analysis or build memory flow. Import SVFG alongside it.
LLVMHistoryResult appendLLVMHistory(TraceFlowGraph &graph, FunctionID id,
                                    const llvm::Function &function,
                                    LLVMHistoryOptions options = {});

/// Supplement a pointer-centric SVFG with integer/scalar SSA data dependencies
/// (e.g. parsed network lengths). Skip loads, stores, calls, atomic memory
/// operations and va_arg: pointer addresses are NOT loaded byte contents and
/// arbitrary argument-to-return flow is not a library summary.
void appendLLVMScalarTransfers(TraceFlowGraph &graph, FunctionID id,
                                const LLVMHistoryResult &history,
                                bool includePointerResults = false);

} // namespace usetracessa
} // namespace lotus
#endif
