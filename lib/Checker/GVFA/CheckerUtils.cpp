/*

Author: rainoftime
*/
#include "Checker/GVFA/CheckerUtils.h"

#include <llvm/IR/Function.h>

using namespace llvm;

namespace CheckerUtils {

void forEachInstruction(Module *M,
                        std::function<void(const Instruction *)> func) {
  for (auto &F : *M) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        func(&I);
      }
    }
  }
}

bool isMemoryAllocation(const CallInst *CI) {
  if (auto *F = CI->getCalledFunction()) {
    StringRef Name = F->getName();
    return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
           Name == "reallocf" || Name == "_Znwm" ||
           Name == "_Znam" ||               // new, new[]
           Name == "_ZnwmRKSt9nothrow_t" || // new(nothrow)
           Name == "_ZnamRKSt9nothrow_t";   // new[](nothrow)
  }
  return false;
}

bool isMemoryDeallocation(const CallInst *CI) {
  if (auto *F = CI->getCalledFunction()) {
    StringRef Name = F->getName();
    return Name == "free" || Name == "cfree" || Name == "delete" ||
           Name == "_ZdlPv" || Name == "_ZdaPv" || Name == "kfree";
  }
  return false;
}

bool doesLibFunctionDereferenceArg(StringRef Name, unsigned ArgIdx) {
  // Memory operations
  if (Name == "memcpy" || Name.startswith("__memcpy_chk") ||
      Name == "memmove" || Name.startswith("__memmove_chk")) {
    return ArgIdx == 0 || ArgIdx == 1;
  }
  if (Name == "memset" || Name.startswith("__memset_chk")) {
    return ArgIdx == 0;
  }

  // String operations
  if (Name == "strcpy" || Name.startswith("__strcpy_chk") ||
      Name == "strncpy" || Name.startswith("__strncpy_chk") ||
      Name == "strcat" || Name.startswith("__strcat_chk") ||
      Name == "strncat" || Name.startswith("__strncat_chk")) {
    return ArgIdx == 0 || ArgIdx == 1;
  }

  if (Name == "strcmp" || Name == "strncmp" || Name == "strlen" ||
      Name == "strnlen") {
    return ArgIdx < 2;
  }

  if (Name == "strchr" || Name == "strrchr" || Name == "strstr") {
    return ArgIdx == 0;
  }

  return false;
}

bool isInitializationFunction(StringRef Name) {
  return Name.contains("init") || Name == "memset" ||
         Name.startswith("__memset_chk") || Name == "bzero" ||
         Name == "explicit_bzero";
}

bool isSafeStackCaptureFunction(StringRef Name) {
  return Name.startswith("llvm.") || Name == "free" || Name == "printf" ||
         Name == "fprintf" || Name == "sprintf" || Name == "snprintf";
}

} // namespace CheckerUtils
