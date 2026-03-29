/**
 * @file TypeHierarchy.h
 * @brief Type Hierarchy Analysis Interface
 *
 * This file defines the interface for type hierarchy analysis, which provides
 * information about class inheritance relationships in object-oriented
 * programs. The analysis enables queries about subtypes, supertypes, and type
 * hierarchies.
 *
 * Key Features:
 * - Subtype relationship queries
 * - Type traversal (get all subtypes of a type)
 * - Type name lookup
 * - JSON output for serialization
 *
 * @author Lotus Analysis Framework
 * @date 2025
 * @ingroup TypeHierarchy
 */

/******************************************************************************
 * Copyright (c) 2019 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

#ifndef LOTUS_ANALYSIS_TYPEHIERARCHY_TYPEHIERARCHY_H
#define LOTUS_ANALYSIS_TYPEHIERARCHY_TYPEHIERARCHY_H

#include "llvm/ADT/Optional.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <vector>

namespace lotus {

/**
 * @template TypeHierarchy<T, F>
 * @brief Abstract interface for type hierarchy analysis
 *
 * This template class defines the interface for querying type hierarchy
 * relationships. Implementations provide concrete analysis of inheritance
 * hierarchies for different type representations.
 *
 * @tparam T The type used to represent individual types (e.g., struct type*)
 * @tparam F The factory type for creating type representations
 *
 * @note Implementations include DIBasedTypeHierarchy for debug info-based
 * analysis
 */
template <typename T, typename F> class TypeHierarchy {
public:
  virtual ~TypeHierarchy() = default;

  /**
   * @brief Check if a type exists in the hierarchy
   * @param Type The type to check
   * @return true if the type is in the hierarchy
   */
  __attribute__((warn_unused_result)) virtual bool hasType(T Type) const = 0;

  /**
   * @brief Check if Type is a subtype of SubType
   * @param Type The potential subtype
   * @param SubType The potential supertype
   * @return true if Type <= SubType in the type hierarchy
   */
  __attribute__((warn_unused_result)) virtual bool isSubType(T Type, T SubType) const = 0;

  /**
   * @brief Get all direct and indirect subtypes of a type
   * @param Type The type to find subtypes for
   * @return Set of all types that are subtypes of Type
   */
  __attribute__((warn_unused_result)) virtual std::set<T> getSubTypes(T Type) const = 0;

  /**
   * @brief Look up a type by its name
   * @param TypeName The name of the type to find
   * @return Optional containing the type if found, empty otherwise
   */
  __attribute__((warn_unused_result)) virtual llvm::Optional<T>
  getType(llvm::StringRef TypeName) const = 0;

  /**
   * @brief Get all types in the hierarchy
   * @return Vector of all types
   */
  __attribute__((warn_unused_result)) virtual std::vector<T> getAllTypes() const = 0;

  /**
   * @brief Get the name of a type
   * @param Type The type to get the name for
   * @return StringRef containing the type name
   */
  __attribute__((warn_unused_result)) virtual llvm::StringRef getTypeName(T Type) const = 0;

  /**
   * @brief Get the number of types in the hierarchy
   * @return Number of types
   */
  __attribute__((warn_unused_result)) virtual size_t size() const noexcept = 0;

  /**
   * @brief Check if the hierarchy is empty
   * @return true if no types are in the hierarchy
   */
  __attribute__((warn_unused_result)) virtual bool empty() const noexcept = 0;

  /**
   * @brief Print the type hierarchy to a stream
   * @param OS The output stream to print to
   */
  virtual void print(llvm::raw_ostream &OS = llvm::outs()) const = 0;

  /**
   * @brief Print the type hierarchy in JSON format
   * @param OS The output stream to print to
   */
  virtual void printAsJson(llvm::raw_ostream &OS) const = 0;
};

/**
 * @brief Output stream operator for TypeHierarchy
 * @param OS The output stream
 * @param TH The type hierarchy to print
 * @return The output stream for chaining
 */
template <typename T, typename F>
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const TypeHierarchy<T, F> &TH) {
  TH.print(OS);
  return OS;
}

} // namespace lotus

#endif
