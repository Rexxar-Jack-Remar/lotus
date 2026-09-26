/** @file LLVMVFTableData.h @brief Data structures backing the LLVM vtable representation. */
/******************************************************************************
 * Copyright (c) 2024 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Maximilian Leo Huber and others
 *****************************************************************************/

#pragma once

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace lotus {
struct LLVMVFTableData {
  std::vector<std::string> VFT;

  LLVMVFTableData() noexcept = default;
  void printAsJson(llvm::raw_ostream &OS) const;

  static LLVMVFTableData deserializeJson(const llvm::Twine &Path);
  static LLVMVFTableData loadJsonString(llvm::StringRef JsonAsString);
};

} // namespace lotus

