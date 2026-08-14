/******************************************************************************
 * Copyright (c) 2023 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Fabian Schiebel and others
 *****************************************************************************/

#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "Analysis/TypeHierarchy/DIBasedTypeHierarchyData.h"
#include "Analysis/TypeHierarchy/LLVMVFTable.h"
#include "Utils/LLVM/Demangle.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace lotus {
using ClassType = DIBasedTypeHierarchy::ClassType;
using IdentifierMap = llvm::StringMap<size_t>;

static const llvm::DIType *stripTypedefs(const llvm::DIType *Type) {
  while (Type && Type->getTag() == llvm::dwarf::DW_TAG_typedef) {
    Type = llvm::cast<llvm::DIDerivedType>(Type)->getBaseType();
  }
  return Type;
}

static std::optional<size_t>
resolveTypeIndex(const llvm::DIType *Type,
                 const llvm::DenseMap<ClassType, size_t> &TypeToVertex,
                 const IdentifierMap &IdentifierToVertex) {
  Type = stripTypedefs(Type);
  if (!Type)
    return std::nullopt;

  auto PointerIt = TypeToVertex.find(Type);
  if (PointerIt != TypeToVertex.end())
    return PointerIt->second;

  const auto *Composite = llvm::dyn_cast<llvm::DICompositeType>(Type);
  if (!Composite || Composite->getIdentifier().empty())
    return std::nullopt;

  auto IdentifierIt = IdentifierToVertex.find(Composite->getIdentifier());
  if (IdentifierIt == IdentifierToVertex.end())
    return std::nullopt;
  return IdentifierIt->second;
}

static std::vector<std::vector<const llvm::Function *>>
buildVTables(const llvm::DebugInfoFinder &DIF,
             llvm::ArrayRef<const llvm::DICompositeType *> VertexTypes,
             const llvm::DenseMap<ClassType, size_t> &TypeToVertex,
             const IdentifierMap &IdentifierToVertex, const llvm::Module &M) {
  std::vector<std::vector<const llvm::Function *>> VT(VertexTypes.size());

  for (const auto *DIFun : DIF.subprograms()) {
    auto Virt = DIFun->getVirtuality();
    if (!Virt) {
      continue;
    }
    auto VIdx = DIFun->getVirtualIndex();
    auto *Parent = llvm::dyn_cast<llvm::DICompositeType>(DIFun->getScope());
    if (!Parent) {
      continue;
    }
    auto ParentIndex =
        resolveTypeIndex(Parent, TypeToVertex, IdentifierToVertex);
    if (!ParentIndex) [[unlikely]] {
      SPDLOG_WARN("Enclosing type '{}' of virtual function '{}' not found in "
                  "the current module",
                  Parent->getName().str(),
                  DemangleUtils::demangle(DIFun->getLinkageName().str()));

      continue;
    }

    const auto *Fun = M.getFunction(DIFun->getLinkageName());
    if (!Fun) {
      SPDLOG_WARN("Referenced virtual function '{}' (aka. {}) not declared in "
                  "the current module",
                  DemangleUtils::demangle(DIFun->getLinkageName().str()),
                  DIFun->getLinkageName().str());
      continue;
    }

    auto &VTable = VT[*ParentIndex];
    if (VTable.size() == VIdx) {
      VTable.push_back(Fun);
    } else {
      if (VTable.size() < VIdx) {
        VTable.resize(VIdx + 1);
      }
      VTable[VIdx] = Fun;
    }
  }
  return VT;
}

struct TypeGraph {
  std::vector<std::vector<uint32_t>> DerivedTypesOf;
};

static TypeGraph
buildTypeGraph(llvm::ArrayRef<const llvm::DICompositeType *> VertexTypes,
               const llvm::DenseMap<ClassType, size_t> &TypeToVertex,
               const IdentifierMap &IdentifierToVertex) {
  TypeGraph TG;
  TG.DerivedTypesOf.resize(VertexTypes.size());

  for (const auto *Composite : VertexTypes) {
    auto DerivedIt = TypeToVertex.find(Composite);
    if (DerivedIt == TypeToVertex.end()) {
      continue;
    }
    auto DerivedIdx = DerivedIt->second;

    for (const auto *Fld : Composite->getElements()) {
      const auto *Inheritenace = llvm::dyn_cast<llvm::DIDerivedType>(Fld);
      if (Inheritenace &&
          Inheritenace->getTag() == llvm::dwarf::DW_TAG_inheritance) {
        const auto *Base = Inheritenace->getBaseType();
        auto BaseIndex =
            resolveTypeIndex(Base, TypeToVertex, IdentifierToVertex);
        if (!BaseIndex) {
          SPDLOG_WARN("Base type '{}' of '{}' could not be resolved",
                      Base ? Base->getName().str() : "<null>",
                      Composite->getName().str());
          continue;
        }

        auto &Derived = TG.DerivedTypesOf[*BaseIndex];
        if (!llvm::is_contained(Derived, DerivedIdx))
          Derived.push_back(DerivedIdx);
      }
    }
  }

  return TG;
}

static llvm::StringRef getCompositeTypeName(const llvm::DICompositeType *Ty) {
  auto Ident = Ty->getIdentifier();
  return Ident.empty() ? Ty->getName() : Ident;
}

DIBasedTypeHierarchy::DIBasedTypeHierarchy(const llvm::Module &M) {
  IdentifierMap IdentifierToVertex;
  // -- Find all types
  {
    llvm::DebugInfoFinder DIF;
    DIF.processModule(M);
    {
      size_t NumTypes = DIF.type_count(); // upper bound

      TypeToVertex.reserve(NumTypes);
      VertexTypes.reserve(NumTypes);
    }

    // -- Filter all struct- or class types

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    static constexpr llvm::dwarf::Tag DwarfTags[] = {
        llvm::dwarf::DW_TAG_class_type,
        llvm::dwarf::DW_TAG_structure_type,
        llvm::dwarf::DW_TAG_union_type,
    };

    for (const auto *Ty : DIF.types()) {
      if (const auto *Composite = llvm::dyn_cast<llvm::DICompositeType>(Ty)) {
        if (!llvm::is_contained(DwarfTags, Composite->getTag())) {
          continue;
        }
        auto Name = getCompositeTypeName(Composite);
        if (Name.empty())
          continue;

        size_t VertexIndex = VertexTypes.size();
        auto Identifier = Composite->getIdentifier();
        if (!Identifier.empty()) {
          auto Existing = IdentifierToVertex.find(Identifier);
          if (Existing != IdentifierToVertex.end()) {
            VertexIndex = Existing->second;
            const auto *Canonical = VertexTypes[VertexIndex];
            if (Canonical->isForwardDecl() && !Composite->isForwardDecl()) {
              VertexTypes[VertexIndex] = Composite;
              NameToType[Name] = Composite;
            }
            TypeToVertex.try_emplace(Composite, VertexIndex);
            continue;
          }
        }

        VertexTypes.push_back(Composite);
        TypeToVertex.try_emplace(Composite, VertexIndex);
        NameToType.try_emplace(Name, Composite);
        if (!Identifier.empty())
          IdentifierToVertex.try_emplace(Identifier, VertexIndex);
      }
    }

    // -- Construct VTables

    auto VT =
        buildVTables(DIF, VertexTypes, TypeToVertex, IdentifierToVertex, M);
    VTables.assign(std::make_move_iterator(VT.begin()),
                   std::make_move_iterator(VT.end()));
  }

  // -- Build a type-graph
  auto TG = buildTypeGraph(VertexTypes, TypeToVertex, IdentifierToVertex);
  DirectDerivedTypes = std::move(TG.DerivedTypesOf);
  ReachabilityCache.resize(VertexTypes.size());
  ReachabilityBits.resize(VertexTypes.size());
}

static const llvm::DICompositeType *
stringToDICompositeType(const llvm::DebugInfoFinder &DIF,
                        llvm::StringRef DITypeName) {
  const llvm::DICompositeType *ByIdentifier = nullptr;
  const llvm::DICompositeType *ByName = nullptr;
  bool HasUniqueByName = false;
  for (const auto *Type : DIF.types()) {
    if (const auto *DICT = llvm::dyn_cast<llvm::DICompositeType>(Type)) {
      auto Ident = DICT->getIdentifier();
      if (Ident == DITypeName) {
        if (!ByIdentifier ||
            (ByIdentifier->isForwardDecl() && !DICT->isForwardDecl())) {
          ByIdentifier = DICT;
        }
        continue;
      }
      if (DICT->getName() == DITypeName) {
        HasUniqueByName = ByName == nullptr;
        ByName = DICT;
      }
    }
  }
  if (ByIdentifier)
    return ByIdentifier;
  if (HasUniqueByName) {
    return ByName;
  }

  llvm::report_fatal_error("DIType doesn't exist: " + DITypeName);
}

DIBasedTypeHierarchy::DIBasedTypeHierarchy(
    const llvm::Module *M, const DIBasedTypeHierarchyData &SerializedData) {
  assert(M && "Cannot deserialize a type hierarchy without a module");
  if (auto Error = SerializedData.validate()) {
    llvm::report_fatal_error(
        llvm::Twine("Invalid serialized type hierarchy: ") +
        llvm::toString(std::move(Error)));
  }

  llvm::DebugInfoFinder DIF;
  DIF.processModule(*M);
  IdentifierMap IdentifierToVertex;

  VertexTypes.reserve(SerializedData.VertexTypes.size());
  TypeToVertex.reserve(SerializedData.VertexTypes.size());
  size_t Idx = 0;
  for (const auto &Curr : SerializedData.VertexTypes) {
    const auto *Ty = stringToDICompositeType(DIF, Curr);
    VertexTypes.push_back(Ty);
    TypeToVertex.try_emplace(Ty, Idx);
    NameToType.try_emplace(Curr, Ty);
    if (!Ty->getIdentifier().empty())
      IdentifierToVertex.try_emplace(Ty->getIdentifier(), Idx);

    ++Idx;
  }

  auto TG = buildTypeGraph(VertexTypes, TypeToVertex, IdentifierToVertex);
  DirectDerivedTypes = std::move(TG.DerivedTypesOf);
  ReachabilityCache.resize(VertexTypes.size());
  ReachabilityBits.resize(VertexTypes.size());
  for (size_t TypeIndex = 0; TypeIndex < VertexTypes.size(); ++TypeIndex) {
    const auto &[Begin, End] = SerializedData.TransitiveDerivedIndex[TypeIndex];
    auto &Cached = ReachabilityCache[TypeIndex];
    Cached.reserve(End - Begin);
    llvm::BitVector Bits(VertexTypes.size());
    for (uint32_t HierarchyIndex = Begin; HierarchyIndex < End;
         ++HierarchyIndex) {
      uint32_t VertexIndex = SerializedData.Hierarchy[HierarchyIndex];
      Cached.push_back(VertexTypes[VertexIndex]);
      Bits.set(VertexIndex);
    }
    ReachabilityBits[TypeIndex].emplace(std::move(Bits));
  }

  for (const auto &Curr : SerializedData.VTables) {
    std::vector<const llvm::Function *> CurrVTable;

    CurrVTable.reserve(Curr.size());
    for (const auto &FuncName : Curr) {
      if (FuncName == LLVMVFTable::NullFunName) {
        CurrVTable.push_back(nullptr);
      } else {
        const auto *Function = M->getFunction(FuncName);
        if (!Function) {
          llvm::report_fatal_error(
              llvm::Twine("Serialized vtable function not found: ") + FuncName);
        }
        CurrVTable.push_back(Function);
      }
    }

    VTables.emplace_back(std::move(CurrVTable));
  }
}

const std::vector<ClassType> &
DIBasedTypeHierarchy::ensureReachability(size_t TypeIdx) const {
  assert(TypeIdx < VertexTypes.size());
  std::lock_guard<std::mutex> Lock(ReachabilityMutex);
  if (ReachabilityBits[TypeIdx])
    return ReachabilityCache[TypeIdx];

  llvm::BitVector Seen(VertexTypes.size());
  llvm::SmallVector<uint32_t> WorkList{static_cast<uint32_t>(TypeIdx)};
  auto &Cached = ReachabilityCache[TypeIdx];
  while (!WorkList.empty()) {
    uint32_t Current = WorkList.pop_back_val();
    if (Seen.test(Current))
      continue;

    Seen.set(Current);
    Cached.push_back(VertexTypes[Current]);
    WorkList.append(DirectDerivedTypes[Current].begin(),
                    DirectDerivedTypes[Current].end());
  }
  ReachabilityBits[TypeIdx].emplace(std::move(Seen));
  return Cached;
}

bool DIBasedTypeHierarchy::isSubType(ClassType BaseType,
                                     ClassType CandidateSubtype) const {
  auto BaseIt = TypeToVertex.find(BaseType);
  auto CandidateIt = TypeToVertex.find(CandidateSubtype);
  if (BaseIt == TypeToVertex.end() || CandidateIt == TypeToVertex.end())
    return false;

  ensureReachability(BaseIt->second);
  return (*ReachabilityBits[BaseIt->second])[CandidateIt->second];
}

auto DIBasedTypeHierarchy::subTypesOf(size_t TypeIdx) const
    -> llvm::iterator_range<const ClassType *> {
  const auto &Cached = ensureReachability(TypeIdx);
  return {Cached.data(), Cached.data() + Cached.size()};
}

auto DIBasedTypeHierarchy::subTypesOf(ClassType Ty) const
    -> llvm::iterator_range<const ClassType *> {
  auto It = TypeToVertex.find(Ty);
  if (It == TypeToVertex.end()) {
    static const ClassType Empty = nullptr;
    return {&Empty, &Empty};
  }

  return subTypesOf(It->second);
}

void DIBasedTypeHierarchy::print(llvm::raw_ostream &OS) const {
  {
    OS << "Type Hierarchy:\n";
    size_t TyIdx = 0;
    for (const auto *Ty : VertexTypes) {
      OS << Ty->getName() << " --> ";
      for (const auto *SubTy : llvm::drop_begin(subTypesOf(TyIdx))) {
        OS << SubTy->getName() << ' ';
      }
      ++TyIdx;
      OS << '\n';
    }
  }

  {
    size_t TyIdx = 0;
    OS << "VFTables:\n";

    for (const auto &VFT : VTables) {
      OS << "Virtual function table for: " << VertexTypes[TyIdx]->getName()
         << '\n';
      for (const auto *F : VFT) {
        OS << "\t-" << (F ? F->getName() : "<null>") << '\n';
      }
      ++TyIdx;
    }
  }
}

DIBasedTypeHierarchyData DIBasedTypeHierarchy::getTypeHierarchyData() const {
  DIBasedTypeHierarchyData Data;

  Data.VertexTypes.reserve(VertexTypes.size());

  for (const auto &Curr : VertexTypes) {
    Data.VertexTypes.push_back(getTypeName(Curr).str());
  }

  if (VertexTypes.size() > std::numeric_limits<uint32_t>::max()) {
    llvm::report_fatal_error("Type hierarchy is too large to serialize");
  }
  Data.TransitiveDerivedIndex.resize(VertexTypes.size());
  for (size_t TypeIndex = 0; TypeIndex < VertexTypes.size(); ++TypeIndex) {
    if (Data.Hierarchy.size() > std::numeric_limits<uint32_t>::max()) {
      llvm::report_fatal_error(
          "Type hierarchy closure is too large to serialize");
    }
    Data.TransitiveDerivedIndex[TypeIndex].first = Data.Hierarchy.size();
    for (ClassType Type : ensureReachability(TypeIndex)) {
      auto VertexIt = TypeToVertex.find(Type);
      assert(VertexIt != TypeToVertex.end());
      Data.Hierarchy.push_back(static_cast<uint32_t>(VertexIt->second));
    }
    if (Data.Hierarchy.size() > std::numeric_limits<uint32_t>::max()) {
      llvm::report_fatal_error(
          "Type hierarchy closure is too large to serialize");
    }
    Data.TransitiveDerivedIndex[TypeIndex].second = Data.Hierarchy.size();
  }

  for (const auto &Curr : VTables) {
    std::vector<std::string> CurrVTableAsString;
    CurrVTableAsString.reserve(Curr.getAllFunctions().size());

    for (const auto &Func : Curr.getAllFunctions()) {
      if (Func) {
        CurrVTableAsString.push_back(Func->getName().str());
        continue;
      }
      CurrVTableAsString.emplace_back(LLVMVFTable::NullFunName);
    }

    Data.VTables.push_back(std::move(CurrVTableAsString));
  }

  return Data;
}

void DIBasedTypeHierarchy::printAsJson(llvm::raw_ostream &OS) const {
  DIBasedTypeHierarchyData Data = getTypeHierarchyData();
  Data.printAsJson(OS);
}

void DIBasedTypeHierarchy::printAsDot(llvm::raw_ostream &OS) const {
  OS << "digraph TypeHierarchy{\n";

  // add nodes
  for (size_t VertexIndex = 0; VertexIndex < VertexTypes.size();
       ++VertexIndex) {
    OS << VertexIndex << "[label=\"";
    OS.write_escaped(VertexTypes[VertexIndex]->getName()) << "\"];\n";
  }

  // add all edges

  for (size_t I = 0; I < DirectDerivedTypes.size(); ++I) {
    for (uint32_t SubType : DirectDerivedTypes[I])
      OS << I << " -> " << SubType << ";\n";
  }

  OS << "}\n";
}

} // namespace lotus
