#pragma once

#include "Alias/InclusionBased/GPG/GPU.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class DataLayout;
class CallBase;
class Function;
class Instruction;
class Module;
class Type;
class Value;
} // namespace llvm

namespace lotus::gpg {

enum class LocationKind : std::uint8_t {
  Use,
  Unknown,
  Null,
  Global,
  Stack,
  Heap,
  Formal,
  SSA,
  Function,
  Return,
};

struct MemoryLocation {
  LocationId id = 0;
  LocationKind kind = LocationKind::Unknown;
  const llvm::Value *value = nullptr;
  const llvm::Function *owner = nullptr;
  const llvm::Type *stored_type = nullptr;
  std::string name;
  bool address_escaped = false;
};

class ProgramModel {
public:
  explicit ProgramModel(llvm::Module &module);

  llvm::Module &module() const { return module_; }
  const llvm::DataLayout &dataLayout() const { return data_layout_; }

  LocationId useLocation() const { return use_location_; }
  LocationId unknownLocation() const { return unknown_location_; }
  LocationId nullLocation() const { return null_location_; }

  LocationId valueLocation(const llvm::Value *value);
  LocationId objectLocation(const llvm::Value *value);
  LocationId returnLocation(const llvm::Function *function);
  LocationId functionLocation(const llvm::Function *function);
  LocationId contextLocation(const llvm::CallBase *call, LocationId original);

  const MemoryLocation *getLocation(LocationId id) const;
  const MemoryLocation *findValueLocation(const llvm::Value *value) const;
  const MemoryLocation *findObjectLocation(const llvm::Value *value) const;
  LocationId locationForValue(const llvm::Value *value) const;
  const std::vector<MemoryLocation> &locations() const { return locations_; }

  StatementId statementId(const llvm::Instruction *instruction) const;
  const llvm::Instruction *instructionFor(StatementId statement) const;

  const llvm::Type *typeAt(LocationId id,
                           const IndirectionList &indirections) const;
  bool typesCompatible(const llvm::Type *lhs, const llvm::Type *rhs) const;
  bool isArrayAccess(const Access &access) const;
  bool forcesWeakUpdate(LocationId id) const;
  bool requiresKLimiting(LocationId id) const;
  void markRequiresKLimiting(LocationId id);
  bool isFunction(LocationId id) const;
  const llvm::Function *asFunction(LocationId id) const;

  Access access(LocationId id, IndirectionList indirections,
                bool upward_exposed = false) const;
  std::string locationName(LocationId id) const;

private:
  llvm::Module &module_;
  const llvm::DataLayout &data_layout_;
  std::vector<MemoryLocation> locations_;
  std::map<const llvm::Value *, LocationId> value_locations_;
  std::map<const llvm::Value *, LocationId> object_locations_;
  std::map<const llvm::Function *, LocationId> return_locations_;
  std::map<std::pair<const llvm::CallBase *, LocationId>, LocationId>
      context_locations_;
  std::map<const llvm::Instruction *, StatementId> statements_;
  std::vector<const llvm::Instruction *> instructions_;
  std::set<LocationId> explicitly_k_limited_locations_;
  LocationId use_location_ = 0;
  LocationId unknown_location_ = 0;
  LocationId null_location_ = 0;

  LocationId addLocation(LocationKind kind, const llvm::Value *value,
                         const llvm::Function *owner,
                         const llvm::Type *stored_type, std::string name,
                         bool address_escaped = false);
  void indexModule();
};

} // namespace lotus::gpg
