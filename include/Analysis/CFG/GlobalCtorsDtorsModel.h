#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"

#include <string>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {

class GlobalCtorsDtorsModel {
public:
  static constexpr llvm::StringLiteral ModelName =
      "__lotusCRuntimeGlobalCtorsModel";

  static constexpr llvm::StringLiteral DtorModelName =
      "__lotusCRuntimeGlobalDtorsModel";

  static constexpr llvm::StringLiteral DtorsCallerName =
      "__lotusGlobalDtorsCaller";

  static constexpr llvm::StringLiteral UserEntrySelectorName =
      "__lotusCRuntimeUserEntrySelector";

  static constexpr llvm::StringLiteral NonDetValuePrefix =
      "__lotusCRuntimeNonDetValue";

  static llvm::Function *
  buildModel(llvm::Module &M,
             llvm::ArrayRef<llvm::Function *> UserEntryPoints);

  static llvm::Function *
  buildModel(llvm::Module &M,
             llvm::ArrayRef<std::string> UserEntryPoints = {"main"});

  [[nodiscard]] static llvm::DenseSet<const llvm::Function *>
  collectGlobalCtorsDtors(const llvm::Module &M);

  [[nodiscard]] static bool isModelGenerated(const llvm::Function &F) noexcept;
};

} // namespace lotus
