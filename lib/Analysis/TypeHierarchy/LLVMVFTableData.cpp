/******************************************************************************
 * Copyright (c) 2024 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Maximilian Leo Huber and others
 *****************************************************************************/

#include "Analysis/TypeHierarchy/LLVMVFTableData.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

namespace lotus {

static LLVMVFTableData getDataFromJson(const std::string &JsonStr) {
  LLVMVFTableData Data;

  auto Parsed = llvm::json::parse(JsonStr);
  if (!Parsed) {
    SPDLOG_ERROR("Failed to parse vtable JSON: {}",
                 llvm::toString(Parsed.takeError()));
    return Data;
  }
  auto *Root = Parsed->getAsObject();
  auto *VFT = Root ? Root->getArray("VFT") : nullptr;
  if (!VFT) {
    SPDLOG_ERROR("Vtable JSON is missing the VFT array");
    return Data;
  }
  Data.VFT.reserve(VFT->size());
  for (const auto &Entry : *VFT) {
    if (auto Value = Entry.getAsString()) {
      Data.VFT.emplace_back(*Value);
    }
  }

  return Data;
}

void LLVMVFTableData::printAsJson(llvm::raw_ostream &OS) const {
  llvm::json::Array VFTJson;
  for (const auto &Function : VFT)
    VFTJson.emplace_back(Function);
  llvm::json::Object Root{{"VFT", std::move(VFTJson)}};
  OS << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Root)));
}

LLVMVFTableData LLVMVFTableData::deserializeJson(const llvm::Twine &Path) {
  std::string PathStr = Path.str();

  std::ifstream file(PathStr);
  if (!file.is_open()) {
    SPDLOG_ERROR("Failed to open file: {}", PathStr);
    return LLVMVFTableData();
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return loadJsonString(buffer.str());
}

LLVMVFTableData LLVMVFTableData::loadJsonString(llvm::StringRef JsonAsString) {
  return getDataFromJson(JsonAsString.str());
}

} // namespace lotus
