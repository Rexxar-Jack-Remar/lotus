#pragma once

#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <string>

namespace lotus {

inline std::string sanitizeZ3Identifier(llvm::StringRef Name) {
  std::string Out;
  Out.reserve(Name.size());
  for (char C : Name) {
    if (std::isalnum(static_cast<unsigned char>(C)) || C == '_') {
      Out.push_back(C);
    } else {
      Out.push_back('_');
    }
  }

  if (Out.empty())
    Out = "v";

  if (std::isdigit(static_cast<unsigned char>(Out.front())))
    Out.insert(0, "v_");

  return Out;
}

inline std::string z3NameForValue(const llvm::Value *V) {
  if (!V)
    return "null";

  if (V->hasName())
    return sanitizeZ3Identifier(V->getName());

  std::string Tmp;
  llvm::raw_string_ostream OS(Tmp);
  V->printAsOperand(OS, /*PrintType=*/false);
  return sanitizeZ3Identifier(OS.str());
}

} // namespace lotus

