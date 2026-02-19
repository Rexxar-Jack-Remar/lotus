//===- AbstractState.cpp -- Abstract State--------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AbstractState.h"

#include "Checker/AE/AbstractInterpretation.h"

#include <algorithm>
#include <functional>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

AbstractState AbstractState::widening(const AbstractState &other) {
  // Match SVF semantics: iterate over existing keys in this, then widen with
  // other
  AbstractState result = *this;
  for (auto it = result._varToAbsVal.begin(); it != result._varToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._varToAbsVal.find(key) != other._varToAbsVal.end()) {
      if (it->second.isInterval() && other._varToAbsVal.at(key).isInterval()) {
        it->second.getInterval().widen_with(
            other._varToAbsVal.at(key).getInterval());
      }
    }
  }
  for (auto it = result._addrToAbsVal.begin(); it != result._addrToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._addrToAbsVal.find(key) != other._addrToAbsVal.end()) {
      if (it->second.isInterval() && other._addrToAbsVal.at(key).isInterval()) {
        it->second.getInterval().widen_with(
            other._addrToAbsVal.at(key).getInterval());
      }
    }
  }
  return result;
}

AbstractState AbstractState::narrowing(const AbstractState &other) {
  // Match SVF semantics: iterate over existing keys in this, then narrow with
  // other
  AbstractState result = *this;
  for (auto it = result._varToAbsVal.begin(); it != result._varToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._varToAbsVal.find(key) != other._varToAbsVal.end()) {
      if (it->second.isInterval() && other._varToAbsVal.at(key).isInterval()) {
        it->second.getInterval().narrow_with(
            other._varToAbsVal.at(key).getInterval());
      }
    }
  }
  for (auto it = result._addrToAbsVal.begin(); it != result._addrToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._addrToAbsVal.find(key) != other._addrToAbsVal.end()) {
      if (it->second.isInterval() && other._addrToAbsVal.at(key).isInterval()) {
        it->second.getInterval().narrow_with(
            other._addrToAbsVal.at(key).getInterval());
      }
    }
  }
  return result;
}

void AbstractState::joinWith(const AbstractState &other) {
  for (const auto &item : other._varToAbsVal) {
    _varToAbsVal[item.first].join_with(item.second);
  }
  for (const auto &item : other._addrToAbsVal) {
    _addrToAbsVal[item.first].join_with(item.second);
  }
  for (const auto &addr : other._freedAddrs) {
    _freedAddrs.insert(addr);
  }
}

void AbstractState::meetWith(const AbstractState &other) {
  for (const auto &item : other._varToAbsVal) {
    auto it = _varToAbsVal.find(item.first);
    if (it != _varToAbsVal.end()) {
      it->second.meet_with(item.second);
    }
  }
  for (const auto &item : other._addrToAbsVal) {
    auto it = _addrToAbsVal.find(item.first);
    if (it != _addrToAbsVal.end()) {
      it->second.meet_with(item.second);
    }
  }
  // Compute intersection of freed addresses
  std::unordered_set<uint32_t> intersection;
  for (uint32_t addr : _freedAddrs) {
    if (other._freedAddrs.find(addr) != other._freedAddrs.end()) {
      intersection.insert(addr);
    }
  }
  _freedAddrs = std::move(intersection);
}

void AbstractState::printAbstractState() const {
  llvm::outs() << "Abstract State:\n";
  llvm::outs() << "  Variables:\n";
  for (const auto &item : _varToAbsVal) {
    llvm::outs() << "    " << item.first << " -> " << item.second.toString()
                 << "\n";
  }
  llvm::outs() << "  Memory:\n";
  for (const auto &item : _addrToAbsVal) {
    llvm::outs() << "    " << item.first << " -> " << item.second.toString()
                 << "\n";
  }
}

bool AbstractState::equals(const AbstractState &other) const {
  return eqVarToValMap(_varToAbsVal, other._varToAbsVal) &&
         eqVarToValMap(_addrToAbsVal, other._addrToAbsVal);
}

uint32_t AbstractState::hash() const {
  // Improved hash function matching SVF's approach
  size_t h = getVarToVal().size() * 2;
  std::hash<uint32_t> hf;
  for (const auto &t : getVarToVal()) {
    h ^= hf(t.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
  }
  size_t h2 = getLocToVal().size() * 2;
  for (const auto &t : getLocToVal()) {
    h2 ^= hf(t.first) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
  }
  // Combine both hashes
  return static_cast<uint32_t>(h ^ (h2 << 1));
}

AbstractValue AbstractState::loadValue(uint32_t varId) {
  AbstractValue result;
  if (inVarToAddrsTable(varId)) {
    for (auto addr : _varToAbsVal[varId].getAddrs()) {
      result.join_with(load(addr));
    }
  }
  return result;
}

void AbstractState::storeValue(uint32_t varId, uint32_t valId) {
  AbstractValue val = _varToAbsVal[valId];
  if (inVarToAddrsTable(varId)) {
    for (auto addr : _varToAbsVal[varId].getAddrs()) {
      store(addr, val);
    }
  }
}

AddressValue AbstractState::getGepObjAddrs(uint32_t pointer,
                                           IntervalValue offset) {
  AddressValue result;
  if (!inVarToAddrsTable(pointer))
    return result;

  // Clamp offset bounds to MaxFieldLimit
  int64_t lb = offset.lb().getIntNumeral();
  int64_t ub = offset.ub().getIntNumeral();
  if (lb < 0)
    lb = 0;
  if (ub > static_cast<int64_t>(MaxFieldLimit))
    ub = MaxFieldLimit;
  if (lb > static_cast<int64_t>(MaxFieldLimit))
    lb = MaxFieldLimit;

  for (auto addr : _varToAbsVal[pointer].getAddrs()) {
    uint32_t objId = getIDFromAddr(addr);
    uint32_t baseAddr = AddressValue::getVirtualMemAddress(objId);

    // Handle offset interval
    if (offset.is_numeral()) {
      int64_t offsetVal = offset.getIntNumeral();
      if (offsetVal >= 0 && offsetVal <= static_cast<int64_t>(MaxFieldLimit)) {
        uint32_t gepAddr = baseAddr + static_cast<uint32_t>(offsetVal);
        result.insert(gepAddr);
      }
    } else {
      // For interval offset, iterate over the range (limited to MaxFieldLimit)
      for (int64_t i = lb; i <= ub && i <= static_cast<int64_t>(MaxFieldLimit);
           ++i) {
        uint32_t gepAddr = baseAddr + static_cast<uint32_t>(i);
        result.insert(gepAddr);
      }
    }
  }
  return result;
}

IntervalValue AbstractState::getByteOffset(const llvm::GetElementPtrInst *gep) {
  llvm::Type *srcType = gep->getSourceElementType();
  IntervalValue offset(0);

  for (const auto *idxIt = gep->idx_begin(); idxIt != gep->idx_end(); ++idxIt) {
    llvm::Value *idx = *idxIt;

    // Get index value
    IntervalValue idxVal(0);
    if (llvm::ConstantInt *cidx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
      idxVal = IntervalValue(cidx->getSExtValue());
    } else {
      // Use stable ID from AbstractInterpretation
      uint32_t idxId = AbstractInterpretation::getValueIdStatic(idx);
      if (inVarToValTable(idxId)) {
        idxVal = _varToAbsVal[idxId].getInterval();
      }
    }

    if (srcType->isArrayTy()) {
      llvm::Type *elemType = srcType->getArrayElementType();
      uint32_t elemSize = elemType->getPrimitiveSizeInBits() / 8;
      if (elemSize == 0)
        elemSize = 8;
      offset = offset + idxVal * IntervalValue(elemSize);
      srcType = elemType;
    } else if (srcType->isStructTy()) {
      llvm::StructType *structType = llvm::cast<llvm::StructType>(srcType);
      if (idxVal.is_numeral()) {
        int64_t idxNum = idxVal.getIntNumeral();
        if (idxNum >= 0 &&
            idxNum < static_cast<int64_t>(structType->getNumElements())) {
          const llvm::StructLayout *layout =
              gep->getModule()->getDataLayout().getStructLayout(structType);
          uint32_t fieldOffset =
              layout->getElementOffset(static_cast<unsigned>(idxNum));
          offset = offset + IntervalValue(fieldOffset);
          srcType = structType->getElementType(static_cast<unsigned>(idxNum));
        }
      }
    } else {
      break;
    }
  }

  return offset;
}

uint32_t AbstractState::getAllocaInstByteSize(const llvm::AllocaInst *alloca) {
  llvm::Type *allocType = alloca->getAllocatedType();
  const llvm::DataLayout &dl = alloca->getModule()->getDataLayout();
  uint32_t typeSize = dl.getTypeAllocSize(allocType);

  // Handle array allocation
  if (alloca->isArrayAllocation()) {
    const llvm::Value *arraySize = alloca->getArraySize();
    if (const llvm::ConstantInt *csize =
            llvm::dyn_cast<llvm::ConstantInt>(arraySize)) {
      return typeSize * static_cast<uint32_t>(csize->getZExtValue());
    } else {
      // Variable-sized array - use MaxFieldLimit as upper bound
      // This matches SVF's behavior for non-constant array sizes
      return typeSize * MaxFieldLimit;
    }
  }

  return typeSize;
}

uint32_t AbstractState::getAllocaInstByteSize(const llvm::AllocaInst *alloca,
                                              const AbstractState &as) {
  llvm::Type *allocType = alloca->getAllocatedType();
  const llvm::DataLayout &dl = alloca->getModule()->getDataLayout();
  uint32_t typeSize = dl.getTypeAllocSize(allocType);

  // Handle array allocation
  if (alloca->isArrayAllocation()) {
    const llvm::Value *arraySize = alloca->getArraySize();
    if (const llvm::ConstantInt *csize =
            llvm::dyn_cast<llvm::ConstantInt>(arraySize)) {
      // Constant array size
      return typeSize * static_cast<uint32_t>(csize->getZExtValue());
    } else {
      // Variable-sized array - try to get size from abstract state
      uint32_t arraySizeId =
          AbstractInterpretation::getValueIdStatic(arraySize);

      if (as.inVarToValTable(arraySizeId)) {
        // Array size is tracked in abstract state
        IntervalValue sizeInterval = as[arraySizeId].getInterval();

        if (!sizeInterval.isBottom() && !sizeInterval.isTop()) {
          // Use upper bound of interval, clamped to MaxFieldLimit
          int64_t ub = sizeInterval.ub().getIntNumeral();
          if (ub < 0) {
            ub = 0;
          }
          if (ub > static_cast<int64_t>(MaxFieldLimit)) {
            ub = MaxFieldLimit;
          }
          // Default element size is 1 (matching SVF's behavior)
          uint32_t elementSize = typeSize > 0 ? typeSize : 1;
          uint64_t res =
              static_cast<uint64_t>(elementSize) * static_cast<uint64_t>(ub);
          // Clamp result to MaxFieldLimit if needed
          if (res > MaxFieldLimit) {
            res = MaxFieldLimit;
          }
          return static_cast<uint32_t>(res);
        }
      }

      // Fallback: use MaxFieldLimit as conservative upper bound
      // This happens when array size is not tracked in abstract state yet
      return typeSize * MaxFieldLimit;
    }
  }

  return typeSize;
}

void AbstractState::initObjVar(const llvm::Value *objVar) {
  // Use stable ID from AbstractInterpretation
  uint32_t varId = AbstractInterpretation::getValueIdStatic(objVar);
  uint32_t objId = varId; // For memory objects, objId == varId

  // Check if it's a global variable
  if (const llvm::GlobalVariable *gv =
          llvm::dyn_cast<llvm::GlobalVariable>(objVar)) {
    if (gv->hasInitializer()) {
      if (const llvm::ConstantInt *ci =
              llvm::dyn_cast<llvm::ConstantInt>(gv->getInitializer())) {
        (*this)[varId] = IntervalValue(ci->getSExtValue(), ci->getSExtValue());
        return;
      } else if (const llvm::ConstantFP *cfp =
                     llvm::dyn_cast<llvm::ConstantFP>(gv->getInitializer())) {
        double val = cfp->getValueAPF().convertToDouble();
        (*this)[varId] = IntervalValue(val, val);
        return;
      } else if (gv->getInitializer()->isNullValue()) {
        (*this)[varId] =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
        return;
      }
    }
    // Global pointer or complex type - track size
    if (gv->getValueType()->isPointerTy()) {
      llvm::Type *pointeeType = gv->getValueType()->getPointerElementType();
      if (pointeeType) {
        const llvm::DataLayout &dl = gv->getParent()->getDataLayout();
        uint32_t size = dl.getTypeAllocSize(pointeeType);
        setObjSize(objId, size);
      }
    }
    (*this)[varId] = AddressValue(getVirtualMemAddress(varId));
    return;
  }

  // Check if it's an alloca instruction
  if (const llvm::AllocaInst *alloca =
          llvm::dyn_cast<llvm::AllocaInst>(objVar)) {
    uint32_t size = getAllocaInstByteSize(alloca);
    setObjSize(objId, size);
    (*this)[varId] = AddressValue(getVirtualMemAddress(varId));
    return;
  }

  // Check if it's a constant
  if (const llvm::ConstantInt *ci = llvm::dyn_cast<llvm::ConstantInt>(objVar)) {
    (*this)[varId] = IntervalValue(ci->getSExtValue(), ci->getSExtValue());
    return;
  }
  if (const llvm::ConstantFP *cfp = llvm::dyn_cast<llvm::ConstantFP>(objVar)) {
    double val = cfp->getValueAPF().convertToDouble();
    (*this)[varId] = IntervalValue(val, val);
    return;
  }
  if (llvm::isa<llvm::ConstantPointerNull>(objVar)) {
    (*this)[varId] =
        IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
    return;
  }

  // For constant arrays/structs, use top
  if (llvm::isa<llvm::ConstantArray>(objVar) ||
      llvm::isa<llvm::ConstantStruct>(objVar)) {
    (*this)[varId] = IntervalValue::top();
    return;
  }

  // Default: treat as memory object with virtual address
  // Try to determine size from type if it's a pointer
  if (objVar->getType()->isPointerTy()) {
    llvm::Type *pointeeType = objVar->getType()->getPointerElementType();
    if (pointeeType && llvm::isa<llvm::Instruction>(objVar)) {
      const llvm::Instruction *inst = llvm::cast<llvm::Instruction>(objVar);
      if (inst->getModule()) {
        const llvm::DataLayout &dl = inst->getModule()->getDataLayout();
        uint32_t size = dl.getTypeAllocSize(pointeeType);
        setObjSize(objId, size);
      }
    }
  }
  (*this)[varId] = AddressValue(getVirtualMemAddress(varId));
}

IntervalValue
AbstractState::getElementIndex(const llvm::GetElementPtrInst *gep) {
  // Check if GEP has constant offset
  if (gep->hasAllConstantIndices()) {
    llvm::APInt offset(64, 0);
    if (gep->accumulateConstantOffset(gep->getModule()->getDataLayout(),
                                      offset)) {
      return IntervalValue(offset.getSExtValue(), offset.getSExtValue());
    }
  }

  IntervalValue res(0);
  llvm::Type *srcType = gep->getSourceElementType();
  const llvm::DataLayout &dl = gep->getModule()->getDataLayout();

  // Iterate over indices in reverse order (matching SVF's behavior)
  for (int i = gep->getNumIndices() - 1; i >= 0; --i) {
    llvm::Value *idx =
        gep->getOperand(i + 1); // +1 because operand 0 is pointer
    llvm::Type *idxType = srcType;

    int64_t idxLb, idxUb;

    // Get index value bounds
    if (llvm::ConstantInt *cidx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
      idxLb = idxUb = cidx->getSExtValue();
    } else {
      // Use stable ID from AbstractInterpretation
      uint32_t idxId = AbstractInterpretation::getValueIdStatic(idx);
      if (inVarToValTable(idxId)) {
        IntervalValue idxItv = (*this)[idxId].getInterval();
        if (idxItv.isBottom()) {
          idxLb = idxUb = 0;
        } else {
          idxLb = idxItv.lb().getIntNumeral();
          idxUb = idxItv.ub().getIntNumeral();
        }
      } else {
        idxLb = 0;
        idxUb = MaxFieldLimit;
      }
    }

    // Adjust bounds based on type
    if (idxType->isPointerTy()) {
      llvm::Type *pointeeType = idxType->getPointerElementType();
      uint32_t elemSize = dl.getTypeAllocSize(pointeeType);
      if (elemSize == 0)
        elemSize = 1;
      uint32_t elemNum = MaxFieldLimit / elemSize;
      if (idxLb > static_cast<int64_t>(elemNum))
        idxLb = MaxFieldLimit;
      else
        idxLb *= elemSize;
      if (idxUb > static_cast<int64_t>(elemNum))
        idxUb = MaxFieldLimit;
      else
        idxUb *= elemSize;
    } else if (idxType->isArrayTy()) {
      uint32_t arraySize = idxType->getArrayNumElements();
      if (idxUb >= static_cast<int64_t>(arraySize) || idxLb < 0) {
        idxLb = idxUb = 0;
      }
      // For arrays, element index is just the index value
    } else if (idxType->isStructTy()) {
      llvm::StructType *structType = llvm::cast<llvm::StructType>(idxType);
      uint32_t numElements = structType->getNumElements();
      if (idxUb >= static_cast<int64_t>(numElements) || idxLb < 0) {
        idxLb = idxUb = 0;
      }
      // For structs, element index is just the field index
    }

    res = res + IntervalValue(idxLb, idxUb);

    // Update srcType for next iteration
    if (idxType->isArrayTy()) {
      srcType = idxType->getArrayElementType();
    } else if (idxType->isStructTy() && idxLb == idxUb && idxLb >= 0) {
      llvm::StructType *structType = llvm::cast<llvm::StructType>(idxType);
      if (static_cast<uint32_t>(idxLb) < structType->getNumElements()) {
        srcType = structType->getElementType(static_cast<unsigned>(idxLb));
      }
    } else if (idxType->isPointerTy()) {
      srcType = idxType->getPointerElementType();
    }
  }

  // Ensure result is within [0, MaxFieldLimit]
  res.meet_with(IntervalValue(static_cast<int64_t>(0),
                              static_cast<int64_t>(MaxFieldLimit)));
  if (res.isBottom()) {
    res = IntervalValue(static_cast<int64_t>(0));
  }
  return res;
}

const llvm::Type *AbstractState::getPointeeElement(uint32_t id) {
  if (inVarToAddrsTable(id)) {
    const AbstractValue &addrs = (*this)[id];
    const AddressValue &addrVal = addrs.getAddrs();
    for (auto addr : addrVal) {
      uint32_t addr_id = getIDFromAddr(addr);
      if (addr_id == 0) // nullptr skip
        continue;

      // Get the LLVM Value from AbstractInterpretation's mapping
      // Use singleton instance to avoid circular dependency
      // Use fully qualified name to avoid shadowing by forward declaration in
      // AbstractState.h
      const llvm::Value *val =
          lotus::analysis::AbstractInterpretation::getAEInstance()
              .getValueFromIdStatic(addr_id);
      if (!val)
        continue;

      // Get the pointee type from LLVM's type system
      llvm::Type *type = val->getType();
      if (type && type->isPointerTy()) {
        return type->getPointerElementType();
      }

      // If val is an AllocaInst, get the allocated type
      if (const llvm::AllocaInst *alloca =
              llvm::dyn_cast<llvm::AllocaInst>(val)) {
        return alloca->getAllocatedType();
      }

      // If val is a GlobalVariable, get the value type
      if (const llvm::GlobalVariable *gv =
              llvm::dyn_cast<llvm::GlobalVariable>(val)) {
        return gv->getValueType();
      }
    }
  }
  return nullptr;
}

} // namespace analysis
} // namespace lotus
