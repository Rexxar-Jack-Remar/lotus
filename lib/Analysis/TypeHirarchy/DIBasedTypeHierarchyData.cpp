/******************************************************************************
 * Copyright (c) 2024 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Maximilian Leo Huber and others
 *****************************************************************************/

#include "Analysis/TypeHirarchy/DIBasedTypeHierarchyData.h"
#include <spdlog/spdlog.h>

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <sstream>

namespace lotus {

static DIBasedTypeHierarchyData getDataFromJson(const std::string &JsonStr) {
  DIBasedTypeHierarchyData Data;

  // Simplified JSON parsing - for production use, consider a proper JSON library
  // This is a basic implementation that handles the expected structure
  std::istringstream iss(JsonStr);
  std::string line;
  std::string currentSection;
  
  while (std::getline(iss, line)) {
    // Parse VertexTypes
    if (line.find("\"VertexTypes\"") != std::string::npos) {
      currentSection = "VertexTypes";
      continue;
    }
    // Parse TransitiveDerivedIndex
    if (line.find("\"TransitiveDerivedIndex\"") != std::string::npos) {
      currentSection = "TransitiveDerivedIndex";
      continue;
    }
    // Parse Hierarchy
    if (line.find("\"Hierarchy\"") != std::string::npos) {
      currentSection = "Hierarchy";
      continue;
    }
    // Parse VTables
    if (line.find("\"VTables\"") != std::string::npos) {
      currentSection = "VTables";
      continue;
    }
    
    // Extract string values
    size_t start = line.find('"');
    if (start != std::string::npos) {
      size_t end = line.find('"', start + 1);
      if (end != std::string::npos) {
        std::string value = line.substr(start + 1, end - start - 1);
        if (currentSection == "VertexTypes") {
          Data.VertexTypes.push_back(value);
        } else if (currentSection == "Hierarchy") {
          Data.Hierarchy.push_back(value);
        }
      }
    }
    
    // Parse pairs for TransitiveDerivedIndex
    if (currentSection == "TransitiveDerivedIndex" && 
        line.find('[') != std::string::npos) {
      // Extract pair values [uint32_t, uint32_t]
      // Simplified - would need proper JSON parsing for production
    }
  }

  return Data;
}

void DIBasedTypeHierarchyData::printAsJson(llvm::raw_ostream &OS) {
  OS << "{\n";
  OS << "  \"VertexTypes\": [\n";
  for (size_t i = 0; i < VertexTypes.size(); ++i) {
    OS << "    \"" << VertexTypes[i] << "\"";
    if (i < VertexTypes.size() - 1) OS << ",";
    OS << "\n";
  }
  OS << "  ],\n";
  
  OS << "  \"TransitiveDerivedIndex\": [\n";
  for (size_t i = 0; i < TransitiveDerivedIndex.size(); ++i) {
    OS << "    [" << TransitiveDerivedIndex[i].first << ", "
       << TransitiveDerivedIndex[i].second << "]";
    if (i < TransitiveDerivedIndex.size() - 1) OS << ",";
    OS << "\n";
  }
  OS << "  ],\n";
  
  OS << "  \"Hierarchy\": [\n";
  for (size_t i = 0; i < Hierarchy.size(); ++i) {
    OS << "    \"" << Hierarchy[i] << "\"";
    if (i < Hierarchy.size() - 1) OS << ",";
    OS << "\n";
  }
  OS << "  ],\n";
  
  OS << "  \"VTables\": [\n";
  for (size_t i = 0; i < VTables.size(); ++i) {
    OS << "    [\n";
    for (size_t j = 0; j < VTables[i].size(); ++j) {
      OS << "      \"" << VTables[i][j] << "\"";
      if (j < VTables[i].size() - 1) OS << ",";
      OS << "\n";
    }
    OS << "    ]";
    if (i < VTables.size() - 1) OS << ",";
    OS << "\n";
  }
  OS << "  ]\n";
  OS << "}\n";
}

DIBasedTypeHierarchyData
DIBasedTypeHierarchyData::deserializeJson(const llvm::Twine &Path) {
  std::string PathStr = Path.str();
  
  std::ifstream file(PathStr);
  if (!file.is_open()) {
    SPDLOG_ERROR("Failed to open file: {}", PathStr);
    return DIBasedTypeHierarchyData();
  }
  
  std::stringstream buffer;
  buffer << file.rdbuf();
  return loadJsonString(buffer.str());
}

DIBasedTypeHierarchyData
DIBasedTypeHierarchyData::loadJsonString(llvm::StringRef JsonAsString) {
  return getDataFromJson(JsonAsString.str());
}

} // namespace lotus
