/** @file SymbolicMemory.h @brief Symbolic access paths and abstract memory objects. */
#pragma once

#include "SymbolicExecution/Core/BigInteger.h"
#include "SymbolicExecution/Core/ProgramVar.h"
#include "SymbolicExecution/Core/PropertyValue.h"
#include "SymbolicExecution/Integration/GVFGUtility.h"

#include <cassert>
#include <string>

namespace SymbolicExecution {
class PTItem;

/// Represents a symbolic access path, base object plus abstract offset.
///
/// AccessPath is the canonical memory designator used throughout the analysis.
/// The base identifies the allocation site or pointer root, while the offset is
/// a PropertyValue that can stay symbolic when exact byte precision is not
/// known.
class AccessPath {
  friend class PTItem;

public:
  AccessPath(const AccessPath &) = default;

  AccessPath(ProgramValuePtr Parent, PropertyValuePtr Offset)
      : Parent(std::move(Parent)), Offset(std::move(Offset)) {}

  AccessPath() : AccessPath(ProgramValuePtr(), PropertyValuePtr()) {}

  AccessPath &operator=(const AccessPath &) = default;

private:
  ProgramValuePtr Parent;
  PropertyValuePtr Offset;

public:
  ProgramValuePtr getParent() const { return Parent; }

  const PropertyValuePtr &getOffset() const { return Offset; }

  bool operator==(const AccessPath &R) const {
    return Parent == R.Parent && Offset == R.Offset;
  }

  bool operator<(const AccessPath &R) const {
    if (!(Parent == R.Parent))
      return Parent < R.Parent;
    return Offset.get() < R.Offset.get();
  }

  bool isEmpty() const {
    if (Parent == nullptr) {
      assert(!Offset);
      return true;
    } else {
      return false;
    }
  }

  size_t hash() const {
    return gvfg_utility::hashHelper({Parent.hash(), Offset.hash()});
  }

  std::string getID() const {
    if (isEmpty()) {
      return "0";
    } else {
      return Parent.getID() + "_" + std::to_string(Offset->hash());
    }
  }
};

/// Describes one abstract memory object reachable by symbolic execution.
///
/// PTItem pairs an access path with object metadata. The kind records whether
/// the object is concrete, symbolic, or a placeholder approximation, and the
/// optional size is used by clients that reason about disjointness, bounds, and
/// memory object reuse.
class PTItem {
public:
  enum MemObjKind { MK_CONCRETE, MK_SYMBOLIC, MK_PLACEHOLDER };

  PTItem(ProgramValuePtr AllocSite, MemObjKind K, int64_t Off = 0,
         const PropertyValuePtr &Sz = PropertyValuePtr());

  PTItem(AccessPath AP, MemObjKind K) : AP(std::move(AP)), Size(), K(K) {}

  bool operator==(const PTItem &R) const {
    return AP == R.AP && Size == R.Size && K == R.K;
  }

  size_t hash() const {
    return gvfg_utility::hashHelper({AP.hash(), Size.hash(), (unsigned)K});
  }

  bool isSymbolic() const;

  bool isConcrete() const;

  bool isPlaceHolder() const;

  bool isOffsetSymbolic() const;

  BigInteger getConstOffset() const;

  ProgramValuePtr getAllocSite() const { return AP.getParent(); }

  PropertyValuePtr getOffset() const { return AP.getOffset(); }

  PropertyValuePtr getSize() const { return Size; }

  AccessPath getAP() const { return AP; }

  PTItem offsetBy(const PropertyValuePtr &Off) const;

  PTItem offsetBy(int64_t Off) const;

  void changeOffsetBy(const PropertyValuePtr &Off);

  std::string getID() const;

private:
  AccessPath AP;
  PropertyValuePtr Size;
  MemObjKind K;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::AccessPath> {
  size_t operator()(const SymbolicExecution::AccessPath &V) const;
};

template <> struct hash<SymbolicExecution::PTItem> {
  size_t operator()(const SymbolicExecution::PTItem &V) const;
};
} // namespace std

