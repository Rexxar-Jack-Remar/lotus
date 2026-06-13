/** @file VFTable.h @brief Abstract vtable base class for virtual function dispatch analysis. */
/******************************************************************************
 * Copyright (c) 2019 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

#ifndef LOTUS_ANALYSIS_TYPEHIERARCHY_VFTABLE_H
#define LOTUS_ANALYSIS_TYPEHIERARCHY_VFTABLE_H

#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace lotus {

/// \brief A generic class to represent a virtual function table
template <typename F> class VFTable {
public:
  virtual ~VFTable() = default;

  __attribute__((warn_unused_result)) virtual F getFunction(unsigned Idx) const = 0;

  __attribute__((warn_unused_result)) virtual std::vector<F> getAllFunctions() const = 0;

  __attribute__((warn_unused_result)) virtual int getIndex(F Func) const = 0;

  __attribute__((warn_unused_result)) virtual bool empty() const = 0;

  __attribute__((warn_unused_result)) virtual size_t size() const = 0;

  virtual void print(llvm::raw_ostream &OS) const = 0;

  virtual void printAsJson(llvm::raw_ostream &OS) const = 0;
};

template <typename T, typename F>
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const VFTable<F> &Table) {
  Table.print(OS);
  return OS;
}

} // namespace lotus

#endif
