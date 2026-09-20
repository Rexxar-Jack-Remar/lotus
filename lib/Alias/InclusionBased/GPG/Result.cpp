#include "Alias/InclusionBased/GPG/Result.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace lotus::gpg {

const AccessSet *
GPGResult::resolvedAccesses(const llvm::Instruction *instruction,
                            const Access &query) const {
  auto instruction_it = points_to_.find(instruction);
  if (instruction_it == points_to_.end())
    return nullptr;
  auto query_it = instruction_it->second.find(query);
  return query_it == instruction_it->second.end() ? nullptr : &query_it->second;
}

std::set<const llvm::Value *>
GPGResult::pointees(const llvm::Instruction *instruction,
                    const llvm::Value *pointer) const {
  std::set<const llvm::Value *> result;
  auto location_it = value_locations_.find(pointer);
  auto instruction_it = points_to_.find(instruction);
  if (location_it == value_locations_.end() ||
      instruction_it == points_to_.end())
    return result;

  for (const auto &[query, resolved] : instruction_it->second) {
    if (query.location != location_it->second)
      continue;
    for (const Access &target : resolved) {
      if (!target.indirections.empty())
        continue;
      auto value_it = location_values_.find(target.location);
      if (value_it != location_values_.end() && value_it->second)
        result.insert(value_it->second);
    }
  }
  return result;
}

std::set<const llvm::Value *>
GPGResult::allPointees(const llvm::Value *pointer) const {
  std::set<const llvm::Value *> result;
  auto location_it = value_locations_.find(pointer);
  if (location_it == value_locations_.end())
    return result;

  for (const auto &[instruction, queries] : points_to_) {
    (void)instruction;
    for (const auto &[query, resolved] : queries) {
      if (query.location != location_it->second)
        continue;
      for (const Access &target : resolved) {
        if (!target.indirections.empty())
          continue;
        auto value_it = location_values_.find(target.location);
        if (value_it != location_values_.end() && value_it->second)
          result.insert(value_it->second);
      }
    }
  }
  return result;
}

const std::set<const llvm::Function *> *
GPGResult::callTargets(const llvm::CallBase *call) const {
  auto found = call_targets_.find(call);
  return found == call_targets_.end() ? nullptr : &found->second;
}

void GPGResult::clear() {
  points_to_.clear();
  call_targets_.clear();
  mod_ref_.clear();
  location_values_.clear();
  location_names_.clear();
  value_locations_.clear();
  stats_ = {};
}

void GPGResult::registerLocation(LocationId id, const llvm::Value *value,
                                 std::string name) {
  location_values_[id] = value;
  location_names_[id] = std::move(name);
}

void GPGResult::registerValue(const llvm::Value *value, LocationId id) {
  if (value && id != 0)
    value_locations_[value] = id;
}

void GPGResult::recordGPU(const GPU &gpu) {
  if (!gpu.origin)
    return;
  for (const GPUQuery &query : gpu.queries) {
    const Access &resolved =
        query.endpoint == QueryEndpoint::Source ? gpu.source : gpu.target;
    points_to_[gpu.origin][query.original].insert(resolved);
  }
}

void GPGResult::recordCallTarget(const llvm::CallBase *call,
                                 const llvm::Function *target) {
  if (call && target)
    call_targets_[call].insert(target);
}

void GPGResult::setModRefSummary(const llvm::Function *function,
                                 ModRefSummary summary) {
  if (function)
    mod_ref_[function] = std::move(summary);
}

} // namespace lotus::gpg
