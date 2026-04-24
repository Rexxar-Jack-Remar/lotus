#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/ConstPointerMap.h"

namespace llvm {
class Type;
} // namespace llvm

namespace tpa {

class PointerLayout;

using PointerLayoutMap = ConstPointerMap<llvm::Type, PointerLayout>;

} // namespace tpa
