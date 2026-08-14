/******************************************************************************
 * Copyright (c) 2024 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Maximilian Leo Huber and others
 *****************************************************************************/

#include "Analysis/TypeHierarchy/DIBasedTypeHierarchyData.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace lotus {

static llvm::Error invalidData(const llvm::Twine &Message) {
  return llvm::createStringError(
      std::make_error_code(std::errc::invalid_argument), Message);
}

static llvm::Expected<uint32_t> getUInt32(const llvm::json::Value &Value,
                                          const llvm::Twine &Field) {
  auto Integer = Value.getAsInteger();
  if (!Integer || *Integer < 0 ||
      static_cast<uint64_t>(*Integer) > std::numeric_limits<uint32_t>::max()) {
    return invalidData(Field + " must be an unsigned 32-bit integer");
  }
  return static_cast<uint32_t>(*Integer);
}

static llvm::Expected<DIBasedTypeHierarchyData>
getDataFromJson(const std::string &JsonStr) {
  DIBasedTypeHierarchyData Data;
  auto Parsed = llvm::json::parse(JsonStr);
  if (!Parsed) {
    return Parsed.takeError();
  }

  auto *Root = Parsed->getAsObject();
  if (!Root) {
    return invalidData("Type hierarchy JSON root is not an object");
  }

  auto *VertexTypes = Root->getArray("VertexTypes");
  if (!VertexTypes)
    return invalidData("VertexTypes must be an array");
  Data.VertexTypes.reserve(VertexTypes->size());
  for (size_t I = 0; I < VertexTypes->size(); ++I) {
    auto Value = (*VertexTypes)[I].getAsString();
    if (!Value) {
      return invalidData(
          llvm::formatv("VertexTypes[{0}] must be a string", I).str());
    }
    Data.VertexTypes.push_back(std::string(*Value));
  }

  auto *TransitiveDerivedIndex = Root->getArray("TransitiveDerivedIndex");
  if (!TransitiveDerivedIndex)
    return invalidData("TransitiveDerivedIndex must be an array");
  Data.TransitiveDerivedIndex.reserve(TransitiveDerivedIndex->size());
  for (size_t I = 0; I < TransitiveDerivedIndex->size(); ++I) {
    auto *Pair = (*TransitiveDerivedIndex)[I].getAsArray();
    if (!Pair || Pair->size() != 2) {
      return invalidData(
          llvm::formatv("TransitiveDerivedIndex[{0}] must be a pair", I).str());
    }
    auto First = getUInt32(
        (*Pair)[0], llvm::formatv("TransitiveDerivedIndex[{0}][0]", I).str());
    if (!First)
      return First.takeError();
    auto Second = getUInt32(
        (*Pair)[1], llvm::formatv("TransitiveDerivedIndex[{0}][1]", I).str());
    if (!Second)
      return Second.takeError();
    Data.TransitiveDerivedIndex.emplace_back(*First, *Second);
  }

  auto *Hierarchy = Root->getArray("Hierarchy");
  if (!Hierarchy)
    return invalidData("Hierarchy must be an array");
  Data.Hierarchy.reserve(Hierarchy->size());
  for (size_t I = 0; I < Hierarchy->size(); ++I) {
    auto TypeIndex =
        getUInt32((*Hierarchy)[I], llvm::formatv("Hierarchy[{0}]", I).str());
    if (!TypeIndex)
      return TypeIndex.takeError();
    Data.Hierarchy.push_back(*TypeIndex);
  }

  auto *VTables = Root->getArray("VTables");
  if (!VTables)
    return invalidData("VTables must be an array");
  Data.VTables.reserve(VTables->size());
  for (size_t I = 0; I < VTables->size(); ++I) {
    auto *VTable = (*VTables)[I].getAsArray();
    if (!VTable) {
      return invalidData(
          llvm::formatv("VTables[{0}] must be an array", I).str());
    }
    std::vector<std::string> Current;
    Current.reserve(VTable->size());
    for (size_t J = 0; J < VTable->size(); ++J) {
      auto Value = (*VTable)[J].getAsString();
      if (!Value) {
        return invalidData(
            llvm::formatv("VTables[{0}][{1}] must be a string", I, J).str());
      }
      Current.push_back(std::string(*Value));
    }
    Data.VTables.emplace_back(std::move(Current));
  }

  if (auto Error = Data.validate())
    return std::move(Error);
  return Data;
}

llvm::Error DIBasedTypeHierarchyData::validate() const {
  if (VertexTypes.size() != TransitiveDerivedIndex.size()) {
    return invalidData("VertexTypes and TransitiveDerivedIndex sizes differ");
  }
  if (VertexTypes.size() != VTables.size()) {
    return invalidData("VertexTypes and VTables sizes differ");
  }

  for (size_t I = 0; I < TransitiveDerivedIndex.size(); ++I) {
    const auto &[Begin, End] = TransitiveDerivedIndex[I];
    if (Begin > End || End > Hierarchy.size()) {
      return invalidData(
          llvm::formatv("Invalid TransitiveDerivedIndex range at index {0}", I)
              .str());
    }
  }
  for (size_t I = 0; I < Hierarchy.size(); ++I) {
    if (Hierarchy[I] >= VertexTypes.size()) {
      return invalidData(
          llvm::formatv("Hierarchy[{0}] is not a valid vertex index", I).str());
    }
  }
  return llvm::Error::success();
}

void DIBasedTypeHierarchyData::printAsJson(llvm::raw_ostream &OS) const {
  llvm::json::Array VertexTypesJson;
  for (const auto &Type : VertexTypes)
    VertexTypesJson.emplace_back(Type);

  llvm::json::Array IndexJson;
  for (const auto &[Begin, End] : TransitiveDerivedIndex)
    IndexJson.push_back(llvm::json::Array{Begin, End});

  llvm::json::Array HierarchyJson;
  for (uint32_t TypeIndex : Hierarchy)
    HierarchyJson.emplace_back(TypeIndex);

  llvm::json::Array VTablesJson;
  for (const auto &VTable : VTables) {
    llvm::json::Array VTableJson;
    for (const auto &Function : VTable)
      VTableJson.emplace_back(Function);
    VTablesJson.push_back(std::move(VTableJson));
  }

  llvm::json::Object Root{
      {"VertexTypes", std::move(VertexTypesJson)},
      {"TransitiveDerivedIndex", std::move(IndexJson)},
      {"Hierarchy", std::move(HierarchyJson)},
      {"VTables", std::move(VTablesJson)},
  };
  OS << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Root)));
}

llvm::Expected<DIBasedTypeHierarchyData>
DIBasedTypeHierarchyData::deserializeJson(const llvm::Twine &Path) {
  std::string PathStr = Path.str();

  std::ifstream file(PathStr);
  if (!file.is_open()) {
    return llvm::createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "Failed to open file: %s", PathStr.c_str());
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return loadJsonString(buffer.str());
}

llvm::Expected<DIBasedTypeHierarchyData>
DIBasedTypeHierarchyData::loadJsonString(llvm::StringRef JsonAsString) {
  return getDataFromJson(JsonAsString.str());
}

} // namespace lotus
