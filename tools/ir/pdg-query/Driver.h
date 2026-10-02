#pragma once

#include "llvm/ADT/Optional.h"

namespace llvm {
class Module;
} // namespace llvm
namespace pdg {
class ProgramGraph;
} // namespace pdg

namespace lotus::pdg_query {
struct Options;

/// Catalog requests and selector validation run before loading the module.
/// None means query execution should proceed; a value is the exit status.
llvm::Optional<int> handleCatalogOptions(const Options &config);
int runQueries(pdg::ProgramGraph &pdg, const llvm::Module &module,
               const Options &config);
} // namespace lotus::pdg_query
