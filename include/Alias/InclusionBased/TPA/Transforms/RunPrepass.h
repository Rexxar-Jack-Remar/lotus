#pragma once

#include <llvm/IR/Module.h>

namespace transform {

void runPrepassOn(llvm::Module &);

} // namespace transform
