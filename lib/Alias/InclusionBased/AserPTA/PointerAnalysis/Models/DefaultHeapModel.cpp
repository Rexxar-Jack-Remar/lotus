/**
 * @file DefaultHeapModel.cpp
 * @brief Default heap model for inferring types of heap-allocated objects.
 *
 * Provides type inference for heap allocation functions (malloc, calloc, etc.)
 * by analyzing subsequent bitcast operations and allocation sizes. This helps
 * create more precise object types for pointer analysis.
 *
 * @author peiming
 */
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/DefaultHeapModel.h"

#include "Alias/InclusionBased/AserPTA/Util/Util.h"

#include <llvm/IR/Instructions.h>
#include <Alias/InclusionBased/AserPTA/PointerAnalysis/Program/CallSite.h>

using namespace llvm;
using namespace aser;

/**
 * @brief Get the destination type of a bitcast following an allocation.
 *
 * Checks if the instruction immediately following an allocation site is a
 * bitcast, and if so, returns the destination type. This helps infer the
 * intended type of heap-allocated objects.
 *
 * @param allocSite The allocation instruction (call/invoke)
 * @return The destination type of the bitcast, or nullptr if not found
 */
static Type *getNextBitCastDestType(const Instruction *allocSite) {
  // a call instruction
  const Instruction *nextInst = nullptr;
  if (auto *call = dyn_cast<CallInst>(allocSite)) {
    nextInst = call->getNextNode();
  } else if (auto *invoke = dyn_cast<InvokeInst>(allocSite)) {
    // skip the exception handler code
    nextInst = invoke->getNormalDest()->getFirstNonPHIOrDbgOrLifetime();
  }

  if (isa_and_nonnull<BitCastInst>(nextInst)) {
    Type *destTy =
        cast<BitCastInst>(nextInst)->getDestTy()->getPointerElementType();
    if (destTy->isSized()) {
      // only when the dest type is sized
      return destTy;
    }
  }

  return nullptr;
}

/**
 * @brief Infer the type of an object allocated by calloc.
 *
 * Analyzes the calloc call site and subsequent bitcast to infer the element
 * type and array size. The signature of calloc is:
 * `void *calloc(size_t elementNum, size_t elementSize)`
 *
 * @param fun The function containing the allocation
 * @param allocSite The calloc call instruction
 * @param numArgNo Argument number for element count
 * @param sizeArgNo Argument number for element size
 * @return Inferred array type, or nullptr if inference fails
 */
Type *DefaultHeapModel::inferCallocType(const Function *fun,
                                        const Instruction *allocSite,
                                        int numArgNo, int sizeArgNo) {
  if (auto *elemType = getNextBitCastDestType(allocSite)) {
    assert(elemType->isSized());

    aser::CallSite CS(allocSite);
    const DataLayout &DL = fun->getParent()->getDataLayout();
    const size_t elemSize = DL.getTypeAllocSize(elemType);
    const Value *elementNum = CS.getArgOperand(numArgNo);
    const Value *elementSize = CS.getArgOperand(sizeArgNo);

    if (auto *size = dyn_cast<ConstantInt>(elementSize)) {
      if (elemSize == size->getSExtValue()) {
        // GREAT, we are sure that the element type is the bitcast type
        if (auto *elemNum = dyn_cast<ConstantInt>(elementNum)) {
          return getBoundedArrayTy(elemType, elemNum->getSExtValue());
        } else {
          // the element number can not be determined
          return getUnboundedArrayTy(elemType);
        }
      }
    }
  }
  return nullptr;
}

/**
 * @brief Infer the type of an object allocated by malloc.
 *
 * Analyzes the malloc call site and subsequent bitcast to infer the allocated
 * type. If the size is known and matches the element type size, returns that
 * type. If the size is a multiple, returns a bounded array. Otherwise returns
 * an unbounded array or nullptr.
 *
 * The signature of malloc is: `void *malloc(size_t elementSize)`
 *
 * @param fun The function containing the allocation
 * @param allocSite The malloc call instruction
 * @param sizeArgNo Argument number for size (-1 to treat as unbounded array)
 * @return Inferred type, or nullptr if inference fails
 */
Type *DefaultHeapModel::inferMallocType(const Function *fun,
                                        const Instruction *allocSite,
                                        int sizeArgNo) {

  if (auto *elemType = getNextBitCastDestType(allocSite)) {
    assert(elemType->isSized());

    // if the sizeArgNo is not specified, treat it as unbounded array
    if (sizeArgNo < 0) {
      return getUnboundedArrayTy(elemType);
    }

    aser::CallSite CS(allocSite);
    const DataLayout &DL = fun->getParent()->getDataLayout();
    const size_t elemSize = DL.getTypeAllocSize(elemType);
    const Value *totalSize = CS.getArgOperand(sizeArgNo);

    // the allocated object size is known statically
    if (auto *constSize = dyn_cast<ConstantInt>(totalSize)) {
      size_t memSize = constSize->getValue().getSExtValue();
      if (memSize == elemSize) {
        // GREAT!
        return elemType;
      } else if (memSize % elemSize == 0) {
        return getBoundedArrayTy(elemType, memSize / elemSize);
      }
      return nullptr;
    } else {
      // the size of allocated heap memory is unknown.
      // treat is an array with infinite elements and the ty
      if (DL.getTypeAllocSize(elemType) == 1) {
        // a int8_t[] is equal to a field-insensitive object.
        return nullptr;
      } else {
        return getUnboundedArrayTy(elemType);
      }
    }
  }

  // we can not resolve the type
  return nullptr;
}
