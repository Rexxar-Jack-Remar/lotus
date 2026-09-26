#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {

class ExternCallbackModel {
public:
  static constexpr llvm::StringLiteral ModelPrefix =
      "__lotusExternCallbackModel";

  static size_t rewriteCalls(llvm::Module &M);

  [[nodiscard]] static bool isModelGenerated(const llvm::Function &F) noexcept;
};

} // namespace lotus
