#include "Alias/InclusionBased/CclyzerAA/CclyzerAA.h"

#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#if defined(LOTUS_HAS_CCLYZERPP) && LOTUS_HAS_CCLYZERPP

#include "PointerAnalysis.h"
#include <boost/flyweight.hpp>
#include <llvm/IR/LegacyPassManager.h>

namespace lotus {
namespace cclyzer {

namespace {

Analysis toCclyzerAnalysis(AnalysisKind kind) {
  switch (kind) {
  case AnalysisKind::Subset:
    return Analysis::SUBSET;
  case AnalysisKind::Unification:
    return Analysis::UNIFICATION;
  case AnalysisKind::Debug:
    return Analysis::DEBUG;
  }
  return Analysis::SUBSET;
}

ContextSensitivity toCclyzerCS(ContextKind kind) {
  switch (kind) {
  case ContextKind::Insensitive:
    return INSENSITIVE;
  case ContextKind::CallSite1:
    return CALLSITE1;
  case ContextKind::CallSite2:
    return CALLSITE2;
  case ContextKind::CallSite3:
    return CALLSITE3;
  case ContextKind::CallSite4:
    return CALLSITE4;
  case ContextKind::CallSite5:
    return CALLSITE5;
  case ContextKind::CallSite6:
    return CALLSITE6;
  case ContextKind::CallSite7:
    return CALLSITE7;
  case ContextKind::CallSite8:
    return CALLSITE8;
  case ContextKind::CallSite9:
    return CALLSITE9;
  case ContextKind::Caller1:
    return CALLER1;
  case ContextKind::Caller2:
    return CALLER2;
  case ContextKind::Caller3:
    return CALLER3;
  case ContextKind::Caller4:
    return CALLER4;
  case ContextKind::Caller5:
    return CALLER5;
  case ContextKind::Caller6:
    return CALLER6;
  case ContextKind::Caller7:
    return CALLER7;
  case ContextKind::Caller8:
    return CALLER8;
  case ContextKind::Caller9:
    return CALLER9;
  }
  return INSENSITIVE;
}

} // namespace

struct CclyzerAA::Impl {
  std::unique_ptr<::cclyzer::LegacyPointerAnalysis> pass;
};

CclyzerAA::CclyzerAA() : _impl(std::make_unique<Impl>()) {}

CclyzerAA::CclyzerAA(const CclyzerOptions &options)
    : _impl(std::make_unique<Impl>()), _options(options) {}

CclyzerAA::~CclyzerAA() = default;

void CclyzerAA::setOptions(const CclyzerOptions &options) {
  _options = options;
}

const CclyzerOptions &CclyzerAA::getOptions() const {
  return _options;
}

bool CclyzerAA::isAvailable() {
  return true;
}

bool CclyzerAA::run(llvm::Module &M) {
  return run(M, _options);
}

bool CclyzerAA::run(llvm::Module &M, const CclyzerOptions &options) {
  _initialized = false;
  _options = options;

  auto pass = std::make_unique<::cclyzer::LegacyPointerAnalysis>();
  pass->setAnalysis(toCclyzerAnalysis(_options.analysis));
  pass->setContextSensitivity(toCclyzerCS(_options.context));
  if (!_options.signaturesPath.empty()) {
    pass->setSignatures(_options.signaturesPath);
  }
  if (!_options.debugDir.empty()) {
    pass->setDebugDir(_options.debugDir);
  }
  pass->setCheckAssertions(_options.checkAssertions);

  llvm::legacy::PassManager PM;
  PM.add(pass.get());
  PM.run(M);

  _impl->pass = std::move(pass);
  _initialized = true;
  return true;
}

llvm::AliasResult CclyzerAA::alias(const llvm::Value *v1,
                                   const llvm::Value *v2) {
  if (!_initialized || !_impl->pass)
    return llvm::AliasResult::MayAlias;
  auto loc1 = llvm::MemoryLocation(
      v1, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
  auto loc2 = llvm::MemoryLocation(
      v2, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
  return alias(loc1, loc2);
}

llvm::AliasResult CclyzerAA::alias(const llvm::MemoryLocation &loc1,
                                   const llvm::MemoryLocation &loc2) {
  if (!_initialized || !_impl->pass)
    return llvm::AliasResult::MayAlias;
  llvm::AAQueryInfo AAQI;
  return _impl->pass->getResult().alias(loc1, loc2, AAQI);
}

bool CclyzerAA::mayAlias(const llvm::Value *v1, const llvm::Value *v2) {
  auto res = alias(v1, v2);
  return res != llvm::AliasResult::NoAlias;
}

bool CclyzerAA::mustAlias(const llvm::Value *v1, const llvm::Value *v2) {
  auto res = alias(v1, v2);
  return res == llvm::AliasResult::MustAlias;
}

bool CclyzerAA::getPointsToSet(const llvm::Value *ptr,
                               std::vector<const llvm::Value *> &ptsSet) {
  ptsSet.clear();
  if (!_initialized || !_impl->pass)
    return false;
  const auto &result = _impl->pass->getResult();
  const auto &varPts = result.getVariablePointsTo();
  const auto &allocSites = result.getAllocationSites();
  std::set<boost::flyweight<std::string>> aliasSets;
  for (const auto &t : varPts) {
    if (std::get<3>(t) == ptr)
      aliasSets.insert(std::get<1>(t));
  }
  for (const auto &t : allocSites) {
    if (aliasSets.count(std::get<3>(t)))
      ptsSet.push_back(std::get<1>(t));
  }
  return true;
}

bool CclyzerAA::isNullPointer(const llvm::Value *ptr) const {
  if (!_initialized || !_impl->pass || !ptr)
    return false;
  const auto &nullSet = _impl->pass->getResult().getNullPtrSet();
  return nullSet.count(ptr) > 0;
}

bool CclyzerAA::getNullPtrSet(std::set<const llvm::Value *> &nulls) const {
  nulls.clear();
  if (!_initialized || !_impl->pass)
    return false;
  nulls = _impl->pass->getResult().getNullPtrSet();
  return true;
}

bool CclyzerAA::getIndirectCallTargets(
    const llvm::Instruction *callSite,
    std::vector<const llvm::Value *> &targets) const {
  targets.clear();
  if (!_initialized || !_impl->pass || !callSite)
    return false;
  const auto &cg = _impl->pass->getResult().getCallGraph();
  auto range = cg.equal_range(callSite);
  for (auto it = range.first; it != range.second; ++it) {
    const llvm::Value *callee = std::get<2>(it->second);
    if (callee) {
      targets.push_back(callee);
    }
  }
  return !targets.empty();
}

} // namespace cclyzer
} // namespace lotus

#else

namespace lotus {
namespace cclyzer {

struct CclyzerAA::Impl {};

CclyzerAA::CclyzerAA() : _impl(std::make_unique<Impl>()) {}

CclyzerAA::CclyzerAA(const CclyzerOptions &options)
    : _impl(std::make_unique<Impl>()), _options(options) {}

CclyzerAA::~CclyzerAA() = default;

void CclyzerAA::setOptions(const CclyzerOptions &options) {
  _options = options;
}

const CclyzerOptions &CclyzerAA::getOptions() const {
  return _options;
}

bool CclyzerAA::isAvailable() {
  return false;
}

bool CclyzerAA::run(llvm::Module &) {
  return false;
}

bool CclyzerAA::run(llvm::Module &, const CclyzerOptions &) {
  return false;
}

llvm::AliasResult CclyzerAA::alias(const llvm::Value *, const llvm::Value *) {
  return llvm::AliasResult::MayAlias;
}

llvm::AliasResult CclyzerAA::alias(const llvm::MemoryLocation &,
                                   const llvm::MemoryLocation &) {
  return llvm::AliasResult::MayAlias;
}

bool CclyzerAA::mayAlias(const llvm::Value *, const llvm::Value *) {
  return true;
}

bool CclyzerAA::mustAlias(const llvm::Value *, const llvm::Value *) {
  return false;
}

bool CclyzerAA::getPointsToSet(const llvm::Value *,
                               std::vector<const llvm::Value *> &ptsSet) {
  ptsSet.clear();
  return false;
}

bool CclyzerAA::isNullPointer(const llvm::Value *) const {
  return false;
}

bool CclyzerAA::getNullPtrSet(std::set<const llvm::Value *> &nulls) const {
  nulls.clear();
  return false;
}

bool CclyzerAA::getIndirectCallTargets(
    const llvm::Instruction *,
    std::vector<const llvm::Value *> &targets) const {
  targets.clear();
  return false;
}

} // namespace cclyzer
} // namespace lotus

#endif
