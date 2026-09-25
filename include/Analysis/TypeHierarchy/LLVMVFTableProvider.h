#pragma once

#include "Analysis/TypeHierarchy/LLVMVFTable.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GlobalVariable.h"

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace llvm {
class Module;
class DIType;
class GlobalVariable;
} // namespace llvm

namespace lotus {

/// \brief A class that provides access to all C++ virtual function tables
/// (VTables) found in the target program.
///
/// Useful for constructing a call graph for a C++-based target.
/// \note This class only works if the target program's IR was generated with
/// debug information.
class LLVMVFTableProvider {
public:
  struct PairHash {
    template <typename T, typename U>
    std::size_t operator()(const std::pair<T, U> &P) const {
      return std::hash<T>()(P.first) ^ (std::hash<U>()(P.second) << 1);
    }
  };

  explicit LLVMVFTableProvider(const llvm::Module &Mod);

  [[nodiscard]] bool hasVFTable(const llvm::DIType *Type) const;
  [[nodiscard]] const LLVMVFTable *getVFTableOrNull(const llvm::DIType *Type,
                                                    uint32_t Index = 0) const;

  [[nodiscard]] const llvm::SmallDenseSet<uint32_t> &
  getVTableIndexInHierarchy(const llvm::DIType *DerivedType,
                            const llvm::DIType *BaseType) const;

  [[nodiscard]] static llvm::StringRef
  removeVTablePrefix(llvm::StringRef GlobName) noexcept;

  [[nodiscard]] static bool isVTable(llvm::StringRef MangledVarName);

  [[nodiscard]] const llvm::GlobalVariable *
  getVFTableGlobal(const llvm::DIType *Type) const;

  [[nodiscard]] const llvm::GlobalVariable *
  getVFTableGlobal(llvm::StringRef ClearTypeName) const;

private:
  llvm::StringMap<const llvm::GlobalVariable *> ClearNameTVMap;
  std::unordered_map<std::pair<const llvm::DIType *, uint32_t>, LLVMVFTable,
                     PairHash>
      TypeVFTMap;
  std::unordered_map<
      const llvm::DIType *,
      llvm::SmallDenseMap<const llvm::DIType *, llvm::SmallDenseSet<uint32_t>>>
      BasesOfVirt;
};

} // namespace lotus
