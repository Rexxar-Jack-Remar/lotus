#include "Concurrency/CUDA/CUDAMemoryModel.h"

#include <optional>

#include <llvm/ADT/SmallPtrSet.h>
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

static const Function *getEnclosingFunction(const Value *value) {
  if (const auto *arg = dyn_cast_or_null<Argument>(value)) {
    return arg->getParent();
  }
  if (const auto *inst = dyn_cast_or_null<Instruction>(value)) {
    return inst->getFunction();
  }
  return nullptr;
}

static const Function *getEnclosingKernel(const Value *value) {
  const Function *function = getEnclosingFunction(value);
  return isNVVMKernel(function) ? function : nullptr;
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

static void appendUniqueBase(const Value *value, BaseObjectInfo &info,
                             SmallPtrSetImpl<const Value *> &seen) {
  if (!value || !seen.insert(value).second) {
    return;
  }
  info.objects.push_back(value);
}

static void appendConservativeKernelBase(const Function *kernel,
                                         BaseObjectInfo &info,
                                         SmallPtrSetImpl<const Value *> &seen) {
  info.unresolved = true;
}

static void collectBaseObjects(const Value *value, BaseObjectInfo &info,
                               SmallPtrSetImpl<const Value *> &seen,
                               const Function *kernel = nullptr,
                               unsigned depth = 0) {
  if (!value || depth > 8) {
    appendConservativeKernelBase(kernel, info, seen);
    info.ambiguous = true;
    return;
  }

  const Value *base = stripCastsAndGEPBase(value);
  if (!base) {
    appendConservativeKernelBase(kernel, info, seen);
    return;
  }

  if (isa<AllocaInst>(base) || isa<GlobalValue>(base)) {
    appendUniqueBase(base, info, seen);
    return;
  }

  if (const auto *arg = dyn_cast<Argument>(base)) {
    appendUniqueBase(base, info, seen);
    return;
  }

  if (const auto *select = dyn_cast<SelectInst>(base)) {
    info.ambiguous = true;
    collectBaseObjects(select->getTrueValue(), info, seen, kernel, depth + 1);
    collectBaseObjects(select->getFalseValue(), info, seen, kernel, depth + 1);
    return;
  }

  if (const auto *phi = dyn_cast<PHINode>(base)) {
    info.ambiguous = true;
    for (const Value *incoming : phi->incoming_values()) {
      collectBaseObjects(incoming, info, seen, kernel, depth + 1);
    }
    return;
  }

  if (const auto *ce = dyn_cast<ConstantExpr>(base)) {
    if (ce->isCast() || ce->getOpcode() == Instruction::GetElementPtr) {
      collectBaseObjects(ce->getOperand(0), info, seen, kernel, depth + 1);
      return;
    }
  }

  if (const auto *inst = dyn_cast<Instruction>(base)) {
    if (const auto *call = dyn_cast<CallBase>(inst)) {
      if (call->getType()->isPointerTy()) {
        appendConservativeKernelBase(kernel, info, seen);
      } else {
        appendUniqueBase(base, info, seen);
      }
      return;
    }

    if (inst->getOpcode() == Instruction::AddrSpaceCast ||
        inst->getOpcode() == Instruction::BitCast ||
        inst->getOpcode() == Instruction::IntToPtr) {
      collectBaseObjects(inst->getOperand(0), info, seen, kernel, depth + 1);
      return;
    }
  }

  appendConservativeKernelBase(kernel, info, seen);
}

static MemorySpaceInfo classifyAddressSpace(unsigned addrspace) {
  switch (addrspace) {
  case 1:
    return {MemorySpace::Global, true, addrspace};
  case 3:
    return {MemorySpace::Shared, true, addrspace};
  case 4:
    return {MemorySpace::Constant, true, addrspace};
  case 5:
    return {MemorySpace::Local, true, addrspace};
  case 7:
    return {MemorySpace::ClusterShared, true, addrspace};
  case 101:
    return {MemorySpace::Device, true, addrspace};
  default:
    return {MemorySpace::Unknown, false, addrspace};
  }
}

static MemorySpaceInfo classifyKernelGenericPointer(const Value *value) {
  const Function *kernel = getEnclosingKernel(value);
  if (!kernel) {
    return {};
  }

  BaseObjectInfo info;
  SmallPtrSet<const Value *, 8> seen;
  collectBaseObjects(value, info, seen, kernel);
  if (info.objects.empty() || info.unresolved) {
    return {};
  }

  std::optional<MemorySpace> inferred;
  for (const Value *object : info.objects) {
    MemorySpace space = MemorySpace::Unknown;
    if (isa<AllocaInst>(object)) {
      space = MemorySpace::Local;
    } else if (isa<Argument>(object)) {
      space = MemorySpace::Global;
    } else if (const auto *gv = dyn_cast<GlobalValue>(object)) {
      space = classifyAddressSpace(gv->getAddressSpace()).space;
      if (space == MemorySpace::Unknown) {
        space = MemorySpace::Host;
      }
    }

    if (space == MemorySpace::Unknown || space == MemorySpace::Host) {
      return {};
    }
    if (!inferred) {
      inferred = space;
      continue;
    }
    if (*inferred != space) {
      return {};
    }
  }

  if (!inferred) {
    return {};
  }
  return {*inferred, false, 0};
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

    return gv->getAddressSpace() == 0
               ? MemorySpaceInfo{MemorySpace::Host, false, 0}
               : MemorySpaceInfo{MemorySpace::Unknown, false,
                                 gv->getAddressSpace()};
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
    if (arg->getParent() && isNVVMKernel(arg->getParent())) {
      if (addrspace == 0) {
        return {MemorySpace::Global, false, addrspace};
      }
      return {MemorySpace::Global, true, addrspace};
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
    if (const Function *kernel = getEnclosingKernel(inst)) {
      if (inst->getType()->isPointerTy() &&
          inst->getType()->getPointerAddressSpace() == 0) {
        MemorySpaceInfo by_base = classifyKernelGenericPointer(value);
        if (by_base.space != MemorySpace::Unknown) {
          return by_base;
        }
        if (inst->getOpcode() == Instruction::IntToPtr) {
          return {};
        }
      }
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

BaseObjectInfo CUDAMemoryModel::getBaseObjectInfo(const Value *value) {
  BaseObjectInfo info;
  SmallPtrSet<const Value *, 8> seen;
  collectBaseObjects(value, info, seen, getEnclosingKernel(value));
  if (info.objects.size() > 1) {
    info.ambiguous = true;
  }
  return info;
}

bool CUDAMemoryModel::isPotentiallyManaged(const Value *value) {
  if (!value) {
    return false;
  }
  const Value *base = getCanonicalBase(value);
  if (!base) {
    return false;
  }
  if (const auto *gv = dyn_cast<GlobalValue>(base)) {
    StringRef name = gv->getName();
    if (name.contains("managed") || name.contains("Managed") ||
        name.contains("UM") || name.contains("um_")) {
      return true;
    }
    if (gv->hasSection()) {
      StringRef section = gv->getSection();
      if (section.contains("managed")) {
        return true;
      }
    }
  }
  if (const auto *arg = dyn_cast<Argument>(base)) {
    StringRef name = arg->getName();
    if (name.contains("managed") || name.contains("Managed") ||
        name.contains("UM") || name.contains("um_")) {
      return true;
    }
  }
  if (const auto *inst = dyn_cast<Instruction>(base)) {
    if (const auto *call = dyn_cast<CallBase>(inst)) {
      if (const Function *callee = call->getCalledFunction()) {
        StringRef name = callee->getName();
        if (name.contains("Managed") || name.contains("cudaMallocManaged") ||
            name.contains("cudaManaged")) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace concurrency::cuda
