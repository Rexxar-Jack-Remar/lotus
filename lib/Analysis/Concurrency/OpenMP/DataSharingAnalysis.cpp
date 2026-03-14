#include "Analysis/Concurrency/OpenMP/DataSharingAnalysis.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace OpenMP {

DataSharingAnalysis::DataSharingAnalysis(Module &module) : m_module(module) {}

void DataSharingAnalysis::analyze() {
  scanGlobalAnnotations();
  for (auto &func : m_module) {
    scanFunctionArguments(func);
  }
}

DataSharingAttribute
DataSharingAnalysis::parseAttribute(const std::string &attr_str) {
  if (attr_str.find("private") != std::string::npos)
    return DataSharingAttribute::Private;
  if (attr_str.find("shared") != std::string::npos)
    return DataSharingAttribute::Shared;
  if (attr_str.find("firstprivate") != std::string::npos)
    return DataSharingAttribute::Firstprivate;
  if (attr_str.find("lastprivate") != std::string::npos)
    return DataSharingAttribute::Lastprivate;
  if (attr_str.find("copyin") != std::string::npos)
    return DataSharingAttribute::Copyin;
  if (attr_str.find("copyout") != std::string::npos)
    return DataSharingAttribute::Copyout;
  if (attr_str.find("linear") != std::string::npos)
    return DataSharingAttribute::Linear;
  if (attr_str.find("reduction") != std::string::npos)
    return DataSharingAttribute::Reduction;
  return DataSharingAttribute::None;
}

void DataSharingAnalysis::scanGlobalAnnotations() {
  NamedMDNode *omp_annotations =
      m_module.getNamedMetadata("llvm.global.annotations");
  if (!omp_annotations)
    return;

  for (unsigned i = 0; i < omp_annotations->getNumOperands(); ++i) {
    MDNode *MA = omp_annotations->getOperand(i);
    if (!MA || MA->getNumOperands() < 2)
      continue;

    Metadata *op0 = MA->getOperand(0).get();
    Metadata *op1 = MA->getOperand(1).get();
    auto *annot = dyn_cast<MDString>(op0);
    auto *var = dyn_cast<ValueAsMetadata>(op1);
    if (!annot || !var)
      continue;

    StringRef annot_str = annot->getString();
    if (!annot_str.startswith("omp ") && !annot_str.startswith("openmp "))
      continue;

    std::string clause = annot_str.substr(4).str();
    DataSharingAttribute attr = parseAttribute(clause);

    const Value *var_val = var->getValue();
    m_variable_attributes[var_val] = attr;
    m_entries.push_back({var_val, attr, clause});
  }
}

void DataSharingAnalysis::scanFunctionArguments(Function &func) {
  for (auto &arg : func.args()) {
    if (arg.hasName()) {
      StringRef arg_name = arg.getName();
      if (arg_name.contains(".omp.")) {
        m_variable_attributes[&arg] = DataSharingAttribute::None;
      }
    }
  }
}

bool DataSharingAnalysis::isPrivate(const Value *v) const {
  auto it = m_variable_attributes.find(v);
  return it != m_variable_attributes.end() &&
         it->second == DataSharingAttribute::Private;
}

bool DataSharingAnalysis::isShared(const Value *v) const {
  auto it = m_variable_attributes.find(v);
  return it != m_variable_attributes.end() &&
         it->second == DataSharingAttribute::Shared;
}

bool DataSharingAnalysis::isFirstprivate(const Value *v) const {
  auto it = m_variable_attributes.find(v);
  return it != m_variable_attributes.end() &&
         it->second == DataSharingAttribute::Firstprivate;
}

DataSharingAttribute DataSharingAnalysis::getAttribute(const Value *v) const {
  auto it = m_variable_attributes.find(v);
  if (it != m_variable_attributes.end())
    return it->second;
  return DataSharingAttribute::None;
}

std::vector<DataSharingEntry>
DataSharingAnalysis::getEntriesForRegion(const Value *region) const {
  std::vector<DataSharingEntry> result;
  for (const auto &entry : m_entries) {
    if (entry.variable == region)
      result.push_back(entry);
  }
  return result;
}
} // namespace OpenMP