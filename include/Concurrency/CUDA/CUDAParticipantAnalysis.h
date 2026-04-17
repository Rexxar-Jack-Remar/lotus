#pragma once

#include "Concurrency/CUDA/CUDAAbstractState.h"
#include "Concurrency/CUDA/CUDASymbolicModel.h"

#include <optional>
#include <vector>

namespace concurrency::cuda {

struct DeviceConfig;

enum class ParticipantRelation {
  Unknown,
  Overlaps,
  Contains,
  ContainedIn,
  Disjoint,
  Equal
};

struct CUDAParticipantPredicate {
  ParticipantRelation relation = ParticipantRelation::Unknown;
  ParticipationScope scope = ParticipationScope::Unknown;
  CUDAParticipantSet lhs;
  CUDAParticipantSet rhs;
  double confidence = 0.0;
};

class CUDAParticipantAnalysis {
public:
  explicit CUDAParticipantAnalysis(const llvm::Function &kernel,
                                   uint32_t warp_size = 32);

  CUDAParticipantSet getActiveParticipants(const llvm::Instruction *inst) const;
  CUDAParticipantSet getActiveSetAt(const llvm::Instruction *inst) const;

  ParticipantRelation computeRelation(const CUDAParticipantSet &lhs,
                                      const CUDAParticipantSet &rhs) const;

  CUDAParticipantPredicate
  evaluatePredicate(const llvm::Instruction *lhs_inst,
                    const llvm::Instruction *rhs_inst) const;

  bool areUniformWithinWarp(const llvm::Value *value) const;
  bool areUniformWithinBlock(const llvm::Value *value) const;
  bool variesPerLane(const llvm::Value *value) const;

private:
  const llvm::Function &m_kernel;
  uint32_t m_warp_size = 32;
};

ParticipantRelation computeOverlap(const CUDAParticipantSet &a,
                                   const CUDAParticipantSet &b);

} // namespace concurrency::cuda