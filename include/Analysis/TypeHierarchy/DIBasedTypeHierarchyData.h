/** @file DIBasedTypeHierarchyData.h @brief Data structures for debug-info-based
 * type hierarchy analysis. */
/******************************************************************************
 * Copyright (c) 2024 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Maximilian Leo Huber and others
 *****************************************************************************/

#ifndef LOTUS_ANALYSIS_TYPEHIERARCHY_DIBASEDTYPEHIERARCHYDATA_H
#define LOTUS_ANALYSIS_TYPEHIERARCHY_DIBASEDTYPEHIERARCHYDATA_H

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lotus {
/// \brief A structure that is used to store already calculated type hierarchy
/// data, serialize that data or deserialize a json file with a previously
/// serialized type hierarchy.
struct DIBasedTypeHierarchyData {
  // Vertex DITypes and llvm::Function * are serialized by name. Hierarchy
  // entries refer to VertexTypes by index, avoiding ambiguous short DI names.

  std::vector<std::string> VertexTypes;
  std::vector<std::pair<uint32_t, uint32_t>> TransitiveDerivedIndex;
  std::vector<uint32_t> Hierarchy;
  std::vector<std::vector<std::string>> VTables;

  DIBasedTypeHierarchyData() noexcept = default;
  void printAsJson(llvm::raw_ostream &OS) const;
  llvm::Error validate() const;

  static llvm::Expected<DIBasedTypeHierarchyData>
  deserializeJson(const llvm::Twine &Path);
  static llvm::Expected<DIBasedTypeHierarchyData>
  loadJsonString(llvm::StringRef JsonAsString);
};

} // namespace lotus

#endif // LOTUS_ANALYSIS_TYPEHIERARCHY_DIBASEDTYPEHIERARCHYDATA_H
