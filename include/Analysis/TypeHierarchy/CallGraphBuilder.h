#pragma once

#include "Analysis/TypeHierarchy/CallGraph.h"
#include "Analysis/TypeHierarchy/CallGraphAnalysisType.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"
#include "Analysis/TypeHierarchy/Resolver.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace llvm {
class Module;
class Function;
} // namespace llvm

namespace lotus {

class DIBasedTypeHierarchy;

CallGraph buildCallGraph(const llvm::Module &M, Resolver &Res,
                         llvm::ArrayRef<const llvm::Function *> EntryPoints);

CallGraph buildCallGraph(const llvm::Module &M, Resolver &Res,
                         llvm::ArrayRef<std::string> EntryPointNames = {"main"});

CallGraph buildCallGraph(const llvm::Module &M, CallGraphAnalysisType CGType,
                         llvm::ArrayRef<std::string> EntryPointNames = {"main"},
                         const DIBasedTypeHierarchy *TH = nullptr,
                         const LLVMVFTableProvider *VTP = nullptr);

std::vector<const llvm::Function *>
getEntryPoints(const llvm::Module &M,
               llvm::ArrayRef<std::string> EntryPointNames = {"main"});

} // namespace lotus
