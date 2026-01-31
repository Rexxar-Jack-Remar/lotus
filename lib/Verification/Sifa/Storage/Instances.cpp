// Explicit instantiations for Storage types used by Sifa and SifaSymAbs.
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/SifaSymAbs.h"
#include "llvm/IR/BasicBlock.h"

template class lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, bool>;
template class lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, lotus::sifa::SymAbsState>;
