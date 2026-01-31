//===-- Verification/Sifa/Caches/TopsortCache.cpp ------------------------===//
//
// Explicit instantiation for TopsortCache<Transition>.
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Caches/TopsortCache.h"

template class lotus::sifa::TopsortCache<lotus::sifa::Transition>;
