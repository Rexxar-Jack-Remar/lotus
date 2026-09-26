/**
 * @file MetadataManager.h
 * @brief Metadata Management for LLVM IR
 *
 * This file provides utilities for managing user-defined metadata attached to
 * LLVM IR elements (modules, loops, instructions, functions, variables).
 * It enables persistent annotation of program elements for analysis and
 * optimization passes.
 *
 * Key Features:
 * - Module-level metadata management
 * - Loop structure metadata
 * - Instruction-level metadata
 * - Function and variable annotations
 * - Source code annotation extraction
 *
 * @author Lotus Analysis Framework
 * @date 2025
 * @ingroup DebugInfo
 */

/*
 * Copyright 2021 - 2022  Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include "Analysis/DebugInfo/MetadataManager/LoopStructure.h"
#include "Analysis/DebugInfo/MetadataManager/MetadataEntry.h"
#include "Utils/LLVM/SystemHeaders.h"

namespace noelle {

/**
 * @class MetadataManager
 * @brief Manages user-defined metadata attached to LLVM IR elements
 *
 * This class provides a unified interface for adding, retrieving, and managing
 * metadata on various LLVM IR elements. It wraps the LLVM metadata system
 * to provide persistent annotations that survive across optimization passes.
 *
 * Metadata can be attached to:
 * - The entire module
 * - Loop structures (using LoopStructure wrapper)
 * - Individual instructions
 * - Functions
 * - Global variables
 *
 * @note Metadata modifications modify the IR code
 * @warning Be cautious when modifying metadata as it affects program semantics
 * @see LoopStructure, MetadataEntry
 */
class MetadataManager {
public:
  /**
   * @brief Construct a MetadataManager for a module
   * @param M The LLVM module to manage metadata for
   */
  MetadataManager(Module &M);

  // ============================================================================
  // Module-Level Metadata
  // ============================================================================

  /**
   * @brief Check if the module has a specific metadata entry
   * @param metadataName The name of the metadata to check
   * @return true if the metadata exists
   */
  bool doesHaveMetadata(const std::string &metadataName) const;

  /**
   * @brief Add metadata to the module
   *
   * Adds a new named metadata entry to the module with the given value.
   *
   * @param metadataName The name of the metadata
   * @param metadataValue The value to store
   *
   * @warning This modifies the IR code
   */
  void addMetadata(const std::string &metadataName,
                   const std::string &metadataValue);

  // ============================================================================
  // Loop Metadata
  // ============================================================================

  /**
   * @brief Loop APIs
   *
   * The following methods provide metadata management for loop structures.
   * These enable annotation of loops with analysis results or transformation
   * information.
   * @{
   */

  /**
   * @brief Check if a loop has a specific metadata entry
   * @param loop The loop structure to check
   * @param metadataName The name of the metadata to check
   * @return true if the metadata exists
   */
  bool doesHaveMetadata(LoopStructure *loop, const std::string &metadataName);

  /**
   * @brief Fetch metadata attached to a loop
   * @param loop The loop structure
   * @param metadataName The name of the metadata
   * @return The metadata value as a string
   */
  std::string getMetadata(LoopStructure *loop, const std::string &metadataName);

  /**
   * @brief Add metadata to a loop
   *
   * @param loop The loop structure
   * @param metadataName The name of the metadata
   * @param metadataValue The value to store
   *
   * @warning This modifies the IR code
   */
  void addMetadata(LoopStructure *loop, const std::string &metadataName,
                   const std::string &metadataValue);

  /**
   * @brief Set an existing metadata entry of a loop
   *
   * Updates an existing metadata value. If the metadata doesn't exist,
   * it will be added.
   *
   * @param loop The loop structure
   * @param metadataName The name of the metadata
   * @param metadataValue The new value
   *
   * @warning This modifies the IR code
   */
  void setMetadata(LoopStructure *loop, const std::string &metadataName,
                   const std::string &metadataValue);

  /**
   * @brief Delete metadata from a loop
   *
   * @param loop The loop structure
   * @param metadataName The name of the metadata to delete
   *
   * @warning This modifies the IR code
   */
  void deleteMetadata(LoopStructure *loop, const std::string &metadataName);

  // ============================================================================
  // Instruction Metadata
  // ============================================================================

  /**
   * @brief Instruction APIs
   *
   * The following methods provide metadata management for individual
   * instructions.
   * @{
   */

  /**
   * @brief Check if an instruction has a specific metadata entry
   * @param inst The instruction to check
   * @param metadataName The name of the metadata to check
   * @return true if the metadata exists
   */
  bool doesHaveMetadata(Instruction *inst, const std::string &metadataName);

  /**
   * @brief Fetch metadata attached to an instruction
   * @param inst The instruction
   * @param metadataName The name of the metadata
   * @return The metadata value as a string
   */
  std::string getMetadata(Instruction *inst, const std::string &metadataName);

  /**
   * @brief Add metadata to an instruction
   *
   * @param inst The instruction
   * @param metadataName The name of the metadata
   * @param metadataValue The value to store
   *
   * @warning This modifies the IR code
   */
  void addMetadata(Instruction *inst, const std::string &metadataName,
                   const std::string &metadataValue);

  /**
   * @brief Set an existing metadata entry of an instruction
   *
   * @param inst The instruction
   * @param metadataName The name of the metadata
   * @param metadataValue The new value
   *
   * @warning This modifies the IR code
   */
  void setMetadata(Instruction *inst, const std::string &metadataName,
                   const std::string &metadataValue);

  /**
   * @brief Delete metadata from an instruction
   *
   * @param inst The instruction
   * @param metadataName The name of the metadata to delete
   *
   * @warning This modifies the IR code
   */
  void deleteMetadata(Instruction *inst, const std::string &metadataName);

  // ============================================================================
  // PDG and Source Code Annotations
  // ============================================================================

  /**
   * @brief Remove all PDG-related metadata
   */
  void removePDGMetadata(void);

  /**
   * @brief Get source code annotations for a function
   * @param f The function to query
   * @return Set of annotation strings
   */
  std::set<std::string> getSourceCodeAnnotations(Function *f) const;

  /**
   * @brief Get source code annotations for a variable
   * @param var The alloca instruction representing the variable
   * @return Set of annotation strings
   */
  std::set<std::string> getSourceCodeAnnotations(AllocaInst *var) const;

  /**
   * @brief Get source code annotations for a global variable
   * @param g The global variable
   * @return Set of annotation strings
   */
  std::set<std::string> getSourceCodeAnnotations(GlobalVariable *g) const;

private:
  Module &program; ///< Reference to the managed module

  /// Map of loop metadata: loop -> (metadata name -> entry)
  std::unordered_map<LoopStructure *,
                     std::unordered_map<std::string, MetadataEntry *>>
      metadata;

  /**
   * @brief Internal method to add loop metadata entry
   * @param loop The loop structure
   * @param metadataName The name of the metadata
   */
  void addMetadata(LoopStructure *loop, const std::string &metadataName);

  std::map<Function *, std::set<std::string>>
      functionMetadata; ///< Function-level annotations
  std::map<AllocaInst *, std::set<std::string>>
      varMetadata; ///< Variable annotations
  std::map<GlobalVariable *, std::set<std::string>>
      globalMetadata; ///< Global variable annotations
};

} // namespace noelle
