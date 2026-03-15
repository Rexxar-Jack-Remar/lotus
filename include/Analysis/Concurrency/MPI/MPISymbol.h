#pragma once

#include <cctype>
#include <string>

#include <llvm/ADT/StringRef.h>

namespace mpi {

inline std::string normalizeMPISymbolName(llvm::StringRef raw_name) {
  llvm::StringRef name = raw_name;
  while (name.startswith("\01")) {
    name = name.drop_front();
  }
  if (name.startswith("_") && !name.startswith("__wrap_")) {
    name = name.drop_front();
  }
  if (name.startswith("__wrap_")) {
    name = name.drop_front(7);
  }
  if (name.startswith("PMPI_")) {
    return std::string("MPI_") + name.drop_front(5).str();
  }
  if (name.startswith("ompi_mpi_")) {
    return std::string("MPI_") + name.drop_front(9).str();
  }
  if (name.startswith("pmpi_")) {
    return std::string("MPI_") + name.drop_front(5).str();
  }
  if (name.startswith("mpi_")) {
    return std::string("MPI_") + name.drop_front(4).str();
  }
  return name.str();
}

inline bool equalsCaseInsensitiveASCII(llvm::StringRef lhs,
                                       llvm::StringRef rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    unsigned char lc = static_cast<unsigned char>(lhs[i]);
    unsigned char rc = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(lc) != std::tolower(rc)) {
      return false;
    }
  }
  return true;
}

} // namespace mpi
