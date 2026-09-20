#include "Alias/InclusionBased/GPG/ProgramModel.h"

#include <algorithm>
#include <sstream>

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus::gpg {

namespace {

bool isAllocatorName(llvm::StringRef name) {
  return name == "malloc" || name == "calloc" || name == "realloc" ||
         name == "aligned_alloc" || name == "valloc" || name == "_Znwm" ||
         name == "_Znam" || name == "_Znwj" || name == "_Znaj";
}

std::string valueName(const llvm::Value *value, llvm::StringRef fallback) {
  if (value && value->hasName())
    return value->getName().str();
  std::string text;
  llvm::raw_string_ostream stream(text);
  if (value)
    value->printAsOperand(stream, false);
  else
    stream << fallback;
  stream.flush();
  return text;
}

const llvm::Type *stripOnePointer(const llvm::Type *type) {
  if (auto *pointer = llvm::dyn_cast_or_null<llvm::PointerType>(type))
    return pointer->getPointerElementType();
  return type;
}

} // namespace

ProgramModel::ProgramModel(llvm::Module &module)
    : module_(module), data_layout_(module.getDataLayout()) {
  locations_.push_back({});
  use_location_ =
      addLocation(LocationKind::Use, nullptr, nullptr, nullptr, "<use>");
  unknown_location_ = addLocation(LocationKind::Unknown, nullptr, nullptr,
                                  nullptr, "<unknown>", true);
  null_location_ =
      addLocation(LocationKind::Null, nullptr, nullptr, nullptr, "<null>");
  indexModule();
}

LocationId ProgramModel::addLocation(LocationKind kind,
                                     const llvm::Value *value,
                                     const llvm::Function *owner,
                                     const llvm::Type *stored_type,
                                     std::string name, bool address_escaped) {
  LocationId id = static_cast<LocationId>(locations_.size());
  locations_.push_back(
      {id, kind, value, owner, stored_type, std::move(name), address_escaped});
  return id;
}

void ProgramModel::indexModule() {
  for (llvm::GlobalVariable &global : module_.globals()) {
    LocationId id =
        addLocation(LocationKind::Global, &global, nullptr,
                    global.getValueType(), valueName(&global, "global"));
    object_locations_[&global] = id;
  }

  for (llvm::Function &function : module_) {
    LocationId function_id =
        addLocation(LocationKind::Function, &function, nullptr,
                    function.getFunctionType(), function.getName().str());
    object_locations_[&function] = function_id;

    LocationId return_id = addLocation(LocationKind::Return, &function,
                                       &function, function.getReturnType(),
                                       function.getName().str() + ".return");
    return_locations_[&function] = return_id;

    for (llvm::Argument &argument : function.args()) {
      if (!argument.getType()->isPointerTy())
        continue;
      LocationId id =
          addLocation(LocationKind::Formal, &argument, &function,
                      argument.getType(), valueName(&argument, "arg"));
      value_locations_[&argument] = id;
    }
  }

  instructions_.push_back(nullptr);
  StatementId next_statement = 1;
  for (llvm::Function &function : module_) {
    if (function.isDeclaration())
      continue;
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      statements_[&instruction] = next_statement++;
      instructions_.push_back(&instruction);

      if (instruction.getType()->isPointerTy())
        (void)valueLocation(&instruction);

      if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
        bool escaped = llvm::PointerMayBeCaptured(alloca, true, true);
        LocationId id = addLocation(LocationKind::Stack, alloca, &function,
                                    alloca->getAllocatedType(),
                                    valueName(alloca, "stack"), escaped);
        object_locations_[alloca] = id;
        continue;
      }

      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (!call || !call->getType()->isPointerTy())
        continue;
      const llvm::Function *callee = call->getCalledFunction();
      if (!callee || !isAllocatorName(callee->getName()))
        continue;
      const llvm::Type *allocated_type = stripOnePointer(call->getType());
      LocationId id =
          addLocation(LocationKind::Heap, call, &function, allocated_type,
                      valueName(call, "heap") + ".heap", true);
      object_locations_[call] = id;
    }
  }
}

LocationId ProgramModel::valueLocation(const llvm::Value *value) {
  if (!value)
    return unknown_location_;
  auto found = value_locations_.find(value);
  if (found != value_locations_.end())
    return found->second;

  const llvm::Function *owner = nullptr;
  LocationKind kind = LocationKind::SSA;
  if (auto *argument = llvm::dyn_cast<llvm::Argument>(value)) {
    owner = argument->getParent();
    kind = LocationKind::Formal;
  } else if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
    owner = instruction->getFunction();
  }
  LocationId id = addLocation(kind, value, owner, value->getType(),
                              valueName(value, "ssa"));
  value_locations_[value] = id;
  return id;
}

LocationId ProgramModel::objectLocation(const llvm::Value *value) {
  if (!value)
    return unknown_location_;
  auto found = object_locations_.find(value);
  return found == object_locations_.end() ? unknown_location_ : found->second;
}

LocationId ProgramModel::returnLocation(const llvm::Function *function) {
  auto found = return_locations_.find(function);
  return found == return_locations_.end() ? unknown_location_ : found->second;
}

LocationId ProgramModel::functionLocation(const llvm::Function *function) {
  return objectLocation(function);
}

LocationId ProgramModel::contextLocation(const llvm::CallBase *call,
                                         LocationId original) {
  if (!call)
    return original;
  const auto key = std::make_pair(call, original);
  auto found = context_locations_.find(key);
  if (found != context_locations_.end())
    return found->second;
  const MemoryLocation *location = getLocation(original);
  if (!location)
    return unknown_location_;
  std::string name = location->name + "@" + std::to_string(statementId(call));
  LocationId id = addLocation(location->kind, nullptr, call->getFunction(),
                              location->stored_type, std::move(name),
                              location->address_escaped);
  context_locations_[key] = id;
  return id;
}

const MemoryLocation *ProgramModel::getLocation(LocationId id) const {
  return id < locations_.size() ? &locations_[id] : nullptr;
}

const MemoryLocation *
ProgramModel::findValueLocation(const llvm::Value *value) const {
  auto found = value_locations_.find(value);
  return found == value_locations_.end() ? nullptr : getLocation(found->second);
}

const MemoryLocation *
ProgramModel::findObjectLocation(const llvm::Value *value) const {
  auto found = object_locations_.find(value);
  return found == object_locations_.end() ? nullptr
                                          : getLocation(found->second);
}

LocationId ProgramModel::locationForValue(const llvm::Value *value) const {
  if (const MemoryLocation *location = findValueLocation(value))
    return location->id;
  if (const MemoryLocation *location = findObjectLocation(value))
    return location->id;
  return 0;
}

StatementId
ProgramModel::statementId(const llvm::Instruction *instruction) const {
  auto found = statements_.find(instruction);
  return found == statements_.end() ? 0 : found->second;
}

const llvm::Instruction *
ProgramModel::instructionFor(StatementId statement) const {
  return statement < instructions_.size() ? instructions_[statement] : nullptr;
}

const llvm::Type *
ProgramModel::typeAt(LocationId id, const IndirectionList &indirections) const {
  const MemoryLocation *location = getLocation(id);
  if (!location)
    return nullptr;
  const llvm::Type *type = location->stored_type;

  for (const Indirection &step : indirections.elements()) {
    if (!type)
      return nullptr;
    switch (step.kind) {
    case IndirectionKind::Dereference:
      type = stripOnePointer(type);
      break;
    case IndirectionKind::Field: {
      type = stripOnePointer(type);
      if (auto *structure = llvm::dyn_cast_or_null<llvm::StructType>(type)) {
        if (step.field < 0 || static_cast<std::uint64_t>(step.field) >=
                                  structure->getNumElements())
          return nullptr;
        type = structure->getElementType(static_cast<unsigned>(step.field));
      } else if (auto *array = llvm::dyn_cast_or_null<llvm::ArrayType>(type)) {
        type = array->getElementType();
      } else if (auto *vector =
                     llvm::dyn_cast_or_null<llvm::VectorType>(type)) {
        type = vector->getElementType();
      } else {
        return nullptr;
      }
      break;
    }
    case IndirectionKind::AnyField:
      return nullptr;
    }
  }
  return type;
}

bool ProgramModel::typesCompatible(const llvm::Type *lhs,
                                   const llvm::Type *rhs) const {
  if (!lhs || !rhs)
    return true;
  if (lhs == rhs)
    return true;
  if (lhs->isPointerTy() && rhs->isPointerTy()) {
    const llvm::Type *left = stripOnePointer(lhs);
    const llvm::Type *right = stripOnePointer(rhs);
    if (!left || !right || left->isIntegerTy(8) || right->isIntegerTy(8))
      return true;
    return typesCompatible(left, right);
  }
  if (lhs->isIntegerTy() && rhs->isIntegerTy())
    return lhs->getIntegerBitWidth() == rhs->getIntegerBitWidth();
  if (auto *left_array = llvm::dyn_cast<llvm::ArrayType>(lhs)) {
    auto *right_array = llvm::dyn_cast<llvm::ArrayType>(rhs);
    return right_array && typesCompatible(left_array->getElementType(),
                                          right_array->getElementType());
  }
  return false;
}

bool ProgramModel::isArrayAccess(const Access &access) const {
  const MemoryLocation *location = getLocation(access.location);
  const llvm::Type *type = location ? location->stored_type : nullptr;
  for (const Indirection &step : access.indirections.elements()) {
    if (!type)
      return false;
    if (step.kind == IndirectionKind::Dereference) {
      type = stripOnePointer(type);
      continue;
    }

    type = stripOnePointer(type);
    if (type && (type->isArrayTy() || type->isVectorTy()))
      return true;
    if (step.kind == IndirectionKind::AnyField)
      return false;
    if (auto *structure = llvm::dyn_cast_or_null<llvm::StructType>(type)) {
      if (step.field < 0 ||
          static_cast<std::uint64_t>(step.field) >= structure->getNumElements())
        return false;
      type = structure->getElementType(static_cast<unsigned>(step.field));
    } else {
      return false;
    }
  }
  return false;
}

bool ProgramModel::forcesWeakUpdate(LocationId id) const {
  const MemoryLocation *location = getLocation(id);
  if (!location)
    return true;
  return location->kind == LocationKind::Heap ||
         location->kind == LocationKind::Unknown || location->address_escaped;
}

bool ProgramModel::requiresKLimiting(LocationId id) const {
  const MemoryLocation *location = getLocation(id);
  if (!location)
    return true;
  return explicitly_k_limited_locations_.count(id) != 0 ||
         location->kind == LocationKind::Heap ||
         location->kind == LocationKind::Unknown ||
         location->kind == LocationKind::Formal ||
         (location->kind == LocationKind::Stack && location->address_escaped);
}

void ProgramModel::markRequiresKLimiting(LocationId id) {
  if (getLocation(id))
    explicitly_k_limited_locations_.insert(id);
}

bool ProgramModel::isFunction(LocationId id) const {
  const MemoryLocation *location = getLocation(id);
  return location && location->kind == LocationKind::Function;
}

const llvm::Function *ProgramModel::asFunction(LocationId id) const {
  const MemoryLocation *location = getLocation(id);
  if (!location || location->kind != LocationKind::Function)
    return nullptr;
  return llvm::dyn_cast_or_null<llvm::Function>(location->value);
}

Access ProgramModel::access(LocationId id, IndirectionList indirections,
                            bool upward_exposed) const {
  const llvm::Type *type = typeAt(id, indirections);
  return {id, std::move(indirections), upward_exposed, type,
          requiresKLimiting(id)};
}

std::string ProgramModel::locationName(LocationId id) const {
  const MemoryLocation *location = getLocation(id);
  if (!location)
    return "<invalid>";
  return location->name;
}

} // namespace lotus::gpg
