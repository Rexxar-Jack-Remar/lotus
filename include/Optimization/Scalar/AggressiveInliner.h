#pragma once

namespace llvm {

class ModulePass;

ModulePass *createAggressiveInlinerPass();

} // namespace llvm
