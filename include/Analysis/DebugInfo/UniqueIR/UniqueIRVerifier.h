/** @file UniqueIRVerifier.h @brief Verifier for unique IR encoded data. */
#pragma once

#include <functional>
#include <set>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Module.h>

namespace lotus {

class UniqueIRVerifier {
public:
  using LoopInfoProvider = std::function<llvm::LoopInfo *(llvm::Function &)>;

  bool verify(const llvm::Module &module,
              LoopInfoProvider get_loop_info = nullptr) const;
};

} // namespace lotus
