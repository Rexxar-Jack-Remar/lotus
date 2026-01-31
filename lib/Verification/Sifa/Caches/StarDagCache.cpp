//===-- Verification/Sifa/Caches/StarDagCache.cpp --------------------------===//
//
// Explicit instantiation for StarDagCache<Transition>.
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Caches/StarDagCache.h"

template class lotus::sifa::StarDagCache<lotus::sifa::Transition>;
