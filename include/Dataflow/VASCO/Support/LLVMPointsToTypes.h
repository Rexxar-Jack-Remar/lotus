#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace vasco {
namespace llvmir {

struct PointsToValue {
  const llvm::Value *Value = nullptr;
  bool IsReturnValue = false;

  static PointsToValue forValue(const llvm::Value *TrackedValue) {
    return PointsToValue{TrackedValue, false};
  }

  static PointsToValue returnValue() { return PointsToValue{nullptr, true}; }

  bool operator<(const PointsToValue &Other) const {
    if (IsReturnValue != Other.IsReturnValue) {
      return IsReturnValue < Other.IsReturnValue;
    }
    return Value < Other.Value;
  }

  bool operator==(const PointsToValue &Other) const {
    return IsReturnValue == Other.IsReturnValue && Value == Other.Value;
  }
};

enum class AllocationSiteKind {
  Stack,
  Heap,
  Global,
  Function,
  Argument,
  Summary,
};

struct AllocationSite {
  AllocationSiteKind Kind = AllocationSiteKind::Summary;
  const llvm::Value *Value = nullptr;
  std::size_t ContextId = 0;

  static AllocationSite stack(const llvm::Value *Site, std::size_t ContextId = 0) {
    return AllocationSite{AllocationSiteKind::Stack, Site, ContextId};
  }

  static AllocationSite heap(const llvm::Value *Site, std::size_t ContextId = 0) {
    return AllocationSite{AllocationSiteKind::Heap, Site, ContextId};
  }

  static AllocationSite global(const llvm::Value *Site) {
    return AllocationSite{AllocationSiteKind::Global, Site, 0};
  }

  static AllocationSite function(const llvm::Value *Site) {
    return AllocationSite{AllocationSiteKind::Function, Site, 0};
  }

  static AllocationSite argument(const llvm::Value *Site, std::size_t ContextId = 0) {
    return AllocationSite{AllocationSiteKind::Argument, Site, ContextId};
  }

  static AllocationSite summary() {
    return AllocationSite{AllocationSiteKind::Summary, nullptr, 0};
  }

  bool isSummary() const { return Kind == AllocationSiteKind::Summary; }

  bool operator<(const AllocationSite &Other) const {
    if (Kind != Other.Kind) {
      return Kind < Other.Kind;
    }
    if (Value != Other.Value) {
      return Value < Other.Value;
    }
    return ContextId < Other.ContextId;
  }

  bool operator==(const AllocationSite &Other) const {
    return Kind == Other.Kind && Value == Other.Value &&
           ContextId == Other.ContextId;
  }
};

struct MemoryLayout {
  const llvm::Type *PointeeType = nullptr;
  bool FieldSensitive = false;
  bool HasKnownSize = false;
  std::uint64_t Size = 0;
  bool CollapsesArrayElements = false;

  static MemoryLayout unknown() { return {}; }

  bool operator<(const MemoryLayout &Other) const {
    if (PointeeType != Other.PointeeType) {
      return PointeeType < Other.PointeeType;
    }
    if (FieldSensitive != Other.FieldSensitive) {
      return FieldSensitive < Other.FieldSensitive;
    }
    if (HasKnownSize != Other.HasKnownSize) {
      return HasKnownSize < Other.HasKnownSize;
    }
    if (Size != Other.Size) {
      return Size < Other.Size;
    }
    return CollapsesArrayElements < Other.CollapsesArrayElements;
  }

  bool operator==(const MemoryLayout &Other) const {
    return PointeeType == Other.PointeeType &&
           FieldSensitive == Other.FieldSensitive &&
           HasKnownSize == Other.HasKnownSize && Size == Other.Size &&
           CollapsesArrayElements == Other.CollapsesArrayElements;
  }
};

struct MemoryBlock {
  AllocationSite Site;
  MemoryLayout Layout;

  static MemoryBlock stack(const llvm::Value *Site,
                           MemoryLayout Layout = MemoryLayout::unknown(),
                           std::size_t ContextId = 0) {
    return MemoryBlock{AllocationSite::stack(Site, ContextId), Layout};
  }

  static MemoryBlock heap(const llvm::Value *Site,
                          MemoryLayout Layout = MemoryLayout::unknown(),
                          std::size_t ContextId = 0) {
    return MemoryBlock{AllocationSite::heap(Site, ContextId), Layout};
  }

  static MemoryBlock global(const llvm::Value *Site,
                            MemoryLayout Layout = MemoryLayout::unknown()) {
    return MemoryBlock{AllocationSite::global(Site), Layout};
  }

  static MemoryBlock function(const llvm::Value *Site) {
    return MemoryBlock{AllocationSite::function(Site), MemoryLayout::unknown()};
  }

  static MemoryBlock argument(const llvm::Value *Site,
                              MemoryLayout Layout = MemoryLayout::unknown(),
                              std::size_t ContextId = 0) {
    return MemoryBlock{AllocationSite::argument(Site, ContextId), Layout};
  }

  static MemoryBlock summary() {
    return MemoryBlock{AllocationSite::summary(), MemoryLayout::unknown()};
  }

  AllocationSiteKind kind() const { return Site.Kind; }
  const llvm::Value *value() const { return Site.Value; }
  std::size_t contextId() const { return Site.ContextId; }
  bool isSummary() const { return Site.Kind == AllocationSiteKind::Summary; }

  bool operator<(const MemoryBlock &Other) const {
    if (Site < Other.Site) {
      return true;
    }
    if (Other.Site < Site) {
      return false;
    }
    return Layout < Other.Layout;
  }

  bool operator==(const MemoryBlock &Other) const {
    return Site == Other.Site && Layout == Other.Layout;
  }
};

using PointsToObject = MemoryBlock;

struct MemoryLocation {
  PointsToObject Object;
  std::int64_t Offset = 0;
  bool IsSummary = false;

  static MemoryLocation exact(PointsToObject Object, std::int64_t Offset = 0) {
    return MemoryLocation{Object, Offset, false};
  }

  static MemoryLocation summary(PointsToObject Object, std::int64_t Offset = 0) {
    return MemoryLocation{Object, Offset, true};
  }

  bool operator<(const MemoryLocation &Other) const {
    if (Object < Other.Object) {
      return true;
    }
    if (Other.Object < Object) {
      return false;
    }
    if (Offset != Other.Offset) {
      return Offset < Other.Offset;
    }
    return IsSummary < Other.IsSummary;
  }

  bool operator==(const MemoryLocation &Other) const {
    return Object == Other.Object && Offset == Other.Offset &&
           IsSummary == Other.IsSummary;
  }
};

using MemoryLocationSet = std::set<MemoryLocation>;
using PointsToSet = std::set<PointsToObject>;

struct PointsToGraph {
  using RootMap = std::map<PointsToValue, MemoryLocationSet>;
  using MemoryMap = std::map<MemoryLocation, MemoryLocationSet>;

  const RootMap &getRoots() const { return Roots; }
  const MemoryMap &getMemory() const { return Memory; }

  const MemoryLocationSet &pointsTo(const PointsToValue &Value) const;
  const MemoryLocationSet &load(const MemoryLocation &Location) const;

  void assign(const PointsToValue &Value, const MemoryLocationSet &Targets);
  void erase(const PointsToValue &Value);
  void clearReturnValue();

  void store(const MemoryLocation &Location, const MemoryLocationSet &Targets,
             bool StrongUpdate);
  void summarizeObject(const PointsToObject &Object);
  void unionRootsFrom(const PointsToGraph &Other);
  void unionMemoryFrom(const PointsToGraph &Other);
  void unionWith(const PointsToGraph &Other);

  bool operator==(const PointsToGraph &Other) const;

private:
  RootMap Roots;
  MemoryMap Memory;
};

std::string formatAllocationSiteKind(AllocationSiteKind Kind);

} // namespace llvmir
} // namespace vasco
