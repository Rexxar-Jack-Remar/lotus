#ifndef ANALYSIS_SYMBOLICEXECUTION_MEMORYAPI_H
#define ANALYSIS_SYMBOLICEXECUTION_MEMORYAPI_H

#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace SymbolicExecution {
class AllocatorAPI {
public:
  AllocatorAPI(std::string Name, std::vector<int> SizeArgs,
               bool ArrayAllocation = false, int GFPFlag = -1,
               bool Zeroed = false, Type *ObjTy = nullptr)
      : Name(std::move(Name)), SizeArgs(std::move(SizeArgs)),
        ArrayAllocation(ArrayAllocation), GFPFlag(GFPFlag), Zeroed(Zeroed),
        ObjTy(ObjTy) {}

  const std::string &getName() const { return Name; }

  const std::vector<int> &getSizeArgs() const { return SizeArgs; }

  bool isArrayAlloc() const { return ArrayAllocation; }

  bool hasValidGFPFlag() const { return GFPFlag != -1; }

  int getGFPFlag() const { return GFPFlag; }

  bool isZeroed() const { return Zeroed; }

  Type *getObjTy() const { return ObjTy; }

  friend class LinuxAllocatorAnalysis;

  std::string toString() const;

  // thread-safe
  static const AllocatorAPI *get(Function *F);

private:
  // `int` value means the argument index, -1 denotes invalid value.
  // if ArrayAllocation, SizeArgs[0] => array num, SizeArgs[1] => size of array
  // element.
  std::string Name;
  std::vector<int> SizeArgs;
  bool ArrayAllocation;
  int GFPFlag;
  bool Zeroed;
  Type *ObjTy;

  AllocatorAPI() {}
};
} // namespace SymbolicExecution

#endif
