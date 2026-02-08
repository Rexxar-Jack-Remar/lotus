#include "Checker/FiTx/Framework_IR/Utils.h"

#include "Checker/FiTx/Core/Instructions.h"
#include "llvm/IR/Instructions.h"

namespace ir_generator {
int getPointerDereferenceNum(llvm::Type* type) {
  int dereference_num = 0;

  while(type->isPointerTy()) {
    type = type->getPointerElementType();
    dereference_num++;
  }

  return dereference_num;
}
}  // namespace ir_generator
