#pragma once

#include "Alias/TPA/PointerAnalysis/Support/PtsMap.h"

namespace tpa {

class Pointer;
using Env = PtsMap<const Pointer *>;

} // namespace tpa