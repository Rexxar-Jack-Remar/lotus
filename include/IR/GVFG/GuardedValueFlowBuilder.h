/// @file GuardedValueFlowBuilder.h
/// @brief Compatibility alias for the structural builder pass
///
/// The builder pass is declared in `GuardedValueFlowGraph.h`.
/// This header provides a shorter, stable alias name for older includes.

#pragma once

// Compatibility shim for older includes. The structural builder pass is
// declared in GuardedValueFlowGraph.h.
#include "IR/GVFG/GuardedValueFlowGraph.h"

namespace lotus {
namespace gvfg {

/// Alias for `GuardedValueFlowGraphBuilderPass`.
using GuardedValueFlowBuilderPass = GuardedValueFlowGraphBuilderPass;

} // namespace gvfg
} // namespace lotus
