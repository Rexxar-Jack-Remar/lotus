#include "Concurrency/CUDA/CUDAMemoryModel.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace concurrency::cuda {

namespace {

static bool isNVVMKernel(const Function *function) {
  return function && (function->hasFnAttribute("nvvm.kernel") ||
                      function->getCallingConv() == CallingConv::PTX_Kernel);
}

static const Value *stripCastsAndGEPBase(const Value *value) {
  const Value *current = value ? value->stripPointerCasts() : nullptr;
  while (current) {
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      current = gep->getPointerOperand()->stripPointerCasts();
      continue;
    }
    if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      if (ce->isCast()) {
        current = ce->getOperand(0)->stripPointerCasts();
        continue;
      }
      if (ce->getOpcode() == Instruction::GetElementPtr) {
        current = ce->getOperand(0)->stripPointerCasts();
        continue;
      }
    }
    break;
  }
  return current;
}

static MemorySpaceInfo classifyAddressSpace(unsigned addrspace) {
  switch (addrspace) {
  case 0:
    return {MemorySpace::Host, true, addrspace};
  case 1:
    return {MemorySpace::Global, true, addrspace};
  case 3:
    return {MemorySpace::Shared, true, addrspace};
  case 4:
    return {MemorySpace::Constant, true, addrspace};
  case 5:
    return {MemorySpace::Local, true, addrspace};
  case 101:
    return {MemorySpace::Device, true, addrspace};
  default:
    return {MemorySpace::Unknown, false, addrspace};
  }
}

static MemorySpaceInfo classifyByName(StringRef name, unsigned addrspace) {
  if (name.contains("shared")) {
    return {MemorySpace::Shared, false, addrspace};
  }
  if (name.contains("constant")) {
    return {MemorySpace::Constant, false, addrspace};
  }
  if (name.contains("device")) {
    return {MemorySpace::Device, false, addrspace};
  }
  if (name.contains("global")) {
    return {MemorySpace::Global, false, addrspace};
  }
  if (name.contains("host")) {
    return {MemorySpace::Host, false, addrspace};
  }
  return {MemorySpace::Unknown, false, addrspace};
}

} // namespace

MemorySpaceInfo CUDAMemoryModel::classify(const Value *value) {
  const Value *base = getCanonicalBase(value);
  if (!base) {
    return {};
  }

  if (isa<AllocaInst>(base)) {
    return {MemorySpace::Local, true, 0};
  }

  if (const auto *gv = dyn_cast<GlobalValue>(base)) {
    if (const auto *ptr_ty = dyn_cast<PointerType>(gv->getType())) {
      MemorySpaceInfo by_as = classifyAddressSpace(ptr_ty->getAddressSpace());
      if (by_as.space != MemorySpace::Unknown) {
        return by_as;
      }
    }
    if (gv->hasSection()) {
      StringRef section = gv->getSection();
      if (section.contains("shared")) {
        return {MemorySpace::Shared, false, gv->getAddressSpace()};
      }
      if (section.contains("constant")) {
        return {MemorySpace::Constant, false, gv->getAddressSpace()};
      }
      if (section.contains("device")) {
        return {MemorySpace::Device, false, gv->getAddressSpace()};
      }
    }

    MemorySpaceInfo by_name =
        classifyByName(gv->getName(), gv->getAddressSpace());
    if (by_name.space != MemorySpace::Unknown) {
      return by_name;
    }
    return {MemorySpace::Host, false, gv->getAddressSpace()};
  }

  if (const auto *arg = dyn_cast<Argument>(base)) {
    const unsigned addrspace = base->getType()->getPointerAddressSpace();
    MemorySpaceInfo by_as = classifyAddressSpace(addrspace);
    if (by_as.space != MemorySpace::Unknown && addrspace != 0) {
      return by_as;
    }
    if (arg->hasByValAttr()) {
      return {MemorySpace::Host, false, addrspace};
    }
    if (arg->hasName()) {
      MemorySpaceInfo by_name = classifyByName(arg->getName(), addrspace);
      if (by_name.space != MemorySpace::Unknown) {
        return by_name;
      }
    }
    if (arg->getParent() && isNVVMKernel(arg->getParent())) {
      return {addrspace == 0 ? MemorySpace::Unknown : MemorySpace::Global, false,
              addrspace};
    }
    return {MemorySpace::Unknown, false, addrspace};
  }

  if (const auto *inst = dyn_cast<Instruction>(base)) {
    if (const auto *ptr_ty = dyn_cast<PointerType>(inst->getType())) {
      MemorySpaceInfo by_as = classifyAddressSpace(ptr_ty->getAddressSpace());
      if (by_as.space != MemorySpace::Unknown) {
        return by_as;
      }
    }
    if (inst->hasName()) {
      return classifyByName(inst->getName(), 0);
    }
  }

  if (const auto *ptr_ty = dyn_cast<PointerType>(base->getType())) {
    MemorySpaceInfo by_as = classifyAddressSpace(ptr_ty->getAddressSpace());
    if (by_as.space != MemorySpace::Unknown) {
      return by_as;
    }
  }

  return {};
}

const Value *CUDAMemoryModel::getCanonicalBase(const Value *value) {
  return stripCastsAndGEPBase(value);
}

} // namespace concurrency::cuda
