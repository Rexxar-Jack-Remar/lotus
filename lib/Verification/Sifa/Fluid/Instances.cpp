// Explicit instantiations for Fluid types used by Sifa and SifaSymAbs.
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/SifaSymAbs.h"

template class lotus::sifa::NeverFluid<bool>;
template class lotus::sifa::NeverFluid<lotus::sifa::SymAbsState>;
