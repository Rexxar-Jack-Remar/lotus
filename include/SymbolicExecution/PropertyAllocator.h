/** @file PropertyAllocator.h @brief Allocator for abstract property values in symbolic execution. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_PROPERTYALLOCATOR_H
#define ANALYSIS_SYMBOLICEXECUTION_PROPERTYALLOCATOR_H

#include "SymbolicExecution/PropertyInteger.h"
#include "SymbolicExecution/PropertySym.h"
#include "SymbolicExecution/PropertyValue.h"

#include <assert.h>

#define ToRaw(Ptr) (Ptr.get())

template <typename Ty, typename... ArgTys>
std::shared_ptr<Ty> GetProperty(ArgTys... Args) {
  return std::shared_ptr<Ty>(new Ty(Args...));
}

template <typename Ty>
bool IsaProperty(const SymbolicExecution::PropertyValuePtr &Val) {
  const SymbolicExecution::PropertyValue *VPtr = Val.get();
  return isa<Ty>(VPtr);
}

template <typename Ty>
const Ty *CastProperty(const SymbolicExecution::PropertyValuePtr &Val) {
  assert(IsaProperty<Ty>(Val));

  const SymbolicExecution::PropertyValue *VPtr = Val.get();
  return cast<Ty>(VPtr);
}

#endif
