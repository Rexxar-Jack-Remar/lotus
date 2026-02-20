//===- SVFIRWrapper.cpp -- SVFIR-like interface using AserPTA ----------//
//
// Implementation of SVFIRWrapper using AserPTA
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/SVFIRWrapper.h"

#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/CallGraph.h"
#include "Alias/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSObject.h"
#include "Alias/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/WavePropagation.h"

namespace lotus {
namespace analysis {

using PTASolver = aser::WavePropagation<aser::DefaultLangModel<
    aser::NoCtx, aser::FSMemModel<aser::NoCtx>, aser::BitVectorPTS>>;
using PTAPass = PointerAnalysisPass<PTASolver>;
using FSObject = aser::FSObject<aser::NoCtx>;

SVFIRWrapper::SVFIRWrapper(void *ptaSolver, llvm::Module *module)
    : ptaSolver_(ptaSolver), module_(module) {}

SVFIRWrapper::~SVFIRWrapper() = default;

void SVFIRWrapper::getPointsTo(const llvm::Value *V,
                               std::vector<void *> &result) const {
  result.clear();
  if (!isPTAReady() || !V || !V->getType()->isPointerTy())
    return;

  auto *pta = static_cast<PTAPass *>(ptaSolver_);
  if (!pta)
    return;

  auto *solver = pta->getPTA();
  if (!solver)
    return;

  // Get points-to objects using the solver - it handles template internally
  // We pass nullptr for context and get FSObject results
  std::vector<const FSObject *> pts;
  solver->getPointsTo(nullptr, V, pts);

  for (const auto *obj : pts) {
    result.push_back(const_cast<FSObject *>(obj));
  }
}

const llvm::Type *SVFIRWrapper::getObjectType(const llvm::Value *V) const {
  if (!isPTAReady() || !V || !V->getType()->isPointerTy())
    return nullptr;

  auto *pta = static_cast<PTAPass *>(ptaSolver_);
  if (!pta)
    return nullptr;

  auto *solver = pta->getPTA();
  if (!solver)
    return nullptr;

  return solver->getPointedType(nullptr, V);
}

bool SVFIRWrapper::alias(const llvm::Value *v1, const llvm::Value *v2) const {
  if (!isPTAReady() || !v1 || !v2)
    return false;

  auto *pta = static_cast<PTAPass *>(ptaSolver_);
  if (!pta)
    return false;

  auto *solver = pta->getPTA();
  if (!solver)
    return false;

  return solver->alias(nullptr, v1, nullptr, v2);
}

const llvm::Function *SVFIRWrapper::getFunction(const std::string &name) const {
  if (module_)
    return module_->getFunction(name);
  return nullptr;
}

} // namespace analysis
} // namespace lotus
