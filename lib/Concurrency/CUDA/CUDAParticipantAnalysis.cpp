#include "Concurrency/CUDA/CUDAParticipantAnalysis.h"

#include "Concurrency/Utils/ThreadAPI.h"

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Constants.h>

namespace concurrency::cuda {

namespace {

ParticipationScope classifyByUniformity(UniformityClass uniformity) {
  switch (uniformity) {
  case UniformityClass::WarpUniform:
    return ParticipationScope::Warp;
  case UniformityClass::BlockUniform:
    return ParticipationScope::Block;
  case UniformityClass::ThreadVarying:
    return ParticipationScope::Grid;
  default:
    return ParticipationScope::Unknown;
  }
}

bool isSynchronizationInstruction(const llvm::Instruction *inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!call) {
    return false;
  }
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api) {
    return false;
  }
  auto type = api->getType(call);
  return type == ThreadAPI::TD_CUDA_BARRIER ||
         type == ThreadAPI::TD_CUDA_WARP_BARRIER ||
         type == ThreadAPI::TD_CUDA_MEMORY_BARRIER ||
         type == ThreadAPI::TD_CUDA_DEVICE_SYNC;
}

void fillInstructionMetadata(CUDAParticipantSet &result,
                             const llvm::Function &kernel,
                             const llvm::Instruction *inst) {
  result.kernel = &kernel;
  result.instruction = inst;
}

bool mustReachInstruction(const llvm::Function &kernel,
                          const llvm::Instruction *inst) {
  if (!inst || kernel.empty()) {
    return false;
  }
  llvm::PostDominatorTree post_dom_tree;
  post_dom_tree.recalculate(const_cast<llvm::Function &>(kernel));
  return post_dom_tree.getNode(inst->getParent()) &&
         post_dom_tree.getNode(&kernel.getEntryBlock()) &&
         post_dom_tree.dominates(inst->getParent(), &kernel.getEntryBlock());
}

} // anonymous namespace

CUDAParticipantAnalysis::CUDAParticipantAnalysis(const llvm::Function &kernel,
                                                 uint32_t warp_size)
    : m_kernel(kernel), m_warp_size(warp_size) {}

CUDAParticipantSet CUDAParticipantAnalysis::getActiveParticipants(
    const llvm::Instruction *inst) const {
  if (!inst) {
    return CUDAParticipantSet{};
  }

  CUDAParticipantSet result;
  fillInstructionMetadata(result, m_kernel, inst);

  if (isSynchronizationInstruction(inst)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
    if (call) {
      ThreadAPI *api = ThreadAPI::getThreadAPI();
      if (api) {
        auto type = api->getType(call);
        if (type == ThreadAPI::TD_CUDA_WARP_BARRIER) {
          result.scopes.push_back(static_cast<int>(ParticipationScope::Warp));
          const bool must_reach = mustReachInstruction(m_kernel, inst);
          const auto *mask = call->arg_empty()
                                 ? nullptr
                                 : llvm::dyn_cast<llvm::ConstantInt>(
                                       call->getArgOperand(0));
          if (!mask || mask->isZero()) {
            result.min_lane = 0;
            result.max_lane = m_warp_size - 1;
            result.is_symbolic = true;
            result.certainty = ParticipantCertainty::Conditional;
            return result;
          }
          const llvm::APInt &value = mask->getValue();
          result.min_lane = std::min<uint32_t>(value.countTrailingZeros(),
                                               m_warp_size - 1);
          result.max_lane = std::min<uint32_t>(value.getActiveBits() - 1,
                                               m_warp_size - 1);
          const bool full_mask =
              value.getBitWidth() >= m_warp_size &&
              value == llvm::APInt::getLowBitsSet(value.getBitWidth(),
                                                  m_warp_size);
          result.is_exact = must_reach;
          result.certainty = !must_reach
                                 ? ParticipantCertainty::Conditional
                                 : (full_mask ? ParticipantCertainty::Exact
                                              : ParticipantCertainty::Partial);
          return result;
        }
        if (type == ThreadAPI::TD_CUDA_BARRIER) {
          result.scopes.push_back(static_cast<int>(ParticipationScope::Block));
          result.min_lane = 0;
          result.max_lane = m_warp_size - 1;
          result.is_exact = mustReachInstruction(m_kernel, inst);
          result.certainty = result.is_exact
                                 ? ParticipantCertainty::Exact
                                 : ParticipantCertainty::Conditional;
          return result;
        }
      }
    }
    result.scopes.push_back(static_cast<int>(ParticipationScope::Grid));
    result.is_exact = false;
    result.certainty = ParticipantCertainty::Conditional;
    return result;
  }

  for (const auto &op : inst->operands()) {
    if (!op) {
      continue;
    }
    auto uniformity = CUDASymbolicModel::classifyUniformity(op);
    auto scope = classifyByUniformity(uniformity);

    bool already_has_scope = false;
    for (int existing : result.scopes) {
      if (existing == static_cast<int>(scope)) {
        already_has_scope = true;
        break;
      }
    }
    if (!already_has_scope) {
      result.scopes.push_back(static_cast<int>(scope));
    }
  }

  if (result.scopes.empty()) {
    result.scopes.push_back(static_cast<int>(ParticipationScope::Grid));
  }

  result.is_symbolic = true;
  if (result.scopes.size() == 1 &&
      result.scopes[0] != static_cast<int>(ParticipationScope::Grid)) {
    result.is_exact = true;
    result.certainty = ParticipantCertainty::Exact;
  } else {
    result.certainty = ParticipantCertainty::Conditional;
  }

  return result;
}

CUDAParticipantSet
CUDAParticipantAnalysis::getActiveSetAt(const llvm::Instruction *inst) const {
  return getActiveParticipants(inst);
}

ParticipantRelation
CUDAParticipantAnalysis::computeRelation(const CUDAParticipantSet &lhs,
                                         const CUDAParticipantSet &rhs) const {
  return computeOverlap(lhs, rhs);
}

CUDAParticipantPredicate CUDAParticipantAnalysis::evaluatePredicate(
    const llvm::Instruction *lhs_inst,
    const llvm::Instruction *rhs_inst) const {
  CUDAParticipantPredicate pred;
  pred.lhs = getActiveParticipants(lhs_inst);
  pred.rhs = getActiveParticipants(rhs_inst);
  pred.relation = computeRelation(pred.lhs, pred.rhs);
  pred.confidence = 1.0;
  return pred;
}

bool CUDAParticipantAnalysis::areUniformWithinWarp(
    const llvm::Value *value) const {
  if (!value) {
    return false;
  }
  return CUDASymbolicModel::classifyUniformity(value) ==
         UniformityClass::WarpUniform;
}

bool CUDAParticipantAnalysis::areUniformWithinBlock(
    const llvm::Value *value) const {
  if (!value) {
    return false;
  }
  return CUDASymbolicModel::classifyUniformity(value) ==
         UniformityClass::BlockUniform;
}

bool CUDAParticipantAnalysis::variesPerLane(const llvm::Value *value) const {
  if (!value) {
    return false;
  }
  return CUDASymbolicModel::classifyUniformity(value) ==
         UniformityClass::ThreadVarying;
}

ParticipantRelation computeOverlap(const CUDAParticipantSet &a,
                                   const CUDAParticipantSet &b) {
  if (a.is_exact && b.is_exact) {
    if (a.min_lane == b.min_lane && a.max_lane == b.max_lane &&
        a.min_block == b.min_block && a.max_block == b.max_block) {
      return ParticipantRelation::Equal;
    }
  }

  bool a_in_block = (a.min_block <= b.max_block && a.max_block >= b.min_block);
  bool b_in_block = (b.min_block <= a.max_block && b.max_block >= a.min_block);

  if (a_in_block && b_in_block) {
    return ParticipantRelation::Overlaps;
  }

  return ParticipantRelation::Unknown;
}

} // namespace concurrency::cuda
