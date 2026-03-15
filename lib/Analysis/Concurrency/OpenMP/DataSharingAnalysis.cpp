#include "Analysis/Concurrency/OpenMP/DataSharingAnalysis.h"

#include <deque>
#include <set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace OpenMP {

DataSharingAnalysis::DataSharingAnalysis(Module &module) : m_module(module) {}

void DataSharingAnalysis::analyze() {
  m_variable_attributes.clear();
  m_region_entries.clear();
  m_entries.clear();
  scanGlobalAnnotations();
  for (auto &func : m_module) {
    scanFunctionArguments(func);
    inferOutlinedFunctionCaptures(func);
  }
}

DataSharingAttribute
DataSharingAnalysis::parseAttribute(const std::string &attr_str) {
  if (attr_str.find("firstprivate") != std::string::npos)
    return DataSharingAttribute::Firstprivate;
  if (attr_str.find("lastprivate") != std::string::npos)
    return DataSharingAttribute::Lastprivate;
  if (attr_str.find("private") != std::string::npos)
    return DataSharingAttribute::Private;
  if (attr_str.find("shared") != std::string::npos)
    return DataSharingAttribute::Shared;
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
  GlobalVariable *annotations =
      m_module.getGlobalVariable("llvm.global.annotations");
  if (!annotations || !annotations->hasInitializer()) {
    return;
  }

  const auto *array = dyn_cast<ConstantArray>(annotations->getInitializer());
  if (!array) {
    return;
  }

  for (unsigned i = 0; i < array->getNumOperands(); ++i) {
    const auto *cs = dyn_cast<ConstantStruct>(array->getOperand(i));
    if (!cs || cs->getNumOperands() < 2) {
      continue;
    }

    const Value *annot_target = cs->getOperand(0)->stripPointerCasts();
    const auto *annot_ptr =
        dyn_cast<GlobalVariable>(cs->getOperand(1)->stripPointerCasts());
    if (!annot_target || !annot_ptr || !annot_ptr->hasInitializer()) {
      continue;
    }

    const auto *annot_data =
        dyn_cast<ConstantDataArray>(annot_ptr->getInitializer());
    if (!annot_data || !annot_data->isString()) {
      continue;
    }

    StringRef annot_str = annot_data->getAsCString();
    if (!annot_str.startswith("omp ") && !annot_str.startswith("openmp ")) {
      continue;
    }

    std::string clause = annot_str.startswith("openmp ")
                             ? annot_str.substr(7).str()
                             : annot_str.substr(4).str();
    DataSharingAttribute attr = parseAttribute(clause);
    addEntry(annot_target, attr, clause);
  }
}

void DataSharingAnalysis::scanFunctionArguments(Function &func) {
  if (func.isDeclaration()) {
    return;
  }

  for (auto &arg : func.args()) {
    if (!arg.hasName()) {
      continue;
    }

    StringRef arg_name = arg.getName();
    if (!arg_name.contains(".omp.")) {
      continue;
    }

    DataSharingAttribute attr = DataSharingAttribute::None;
    std::string clause = "outlined-capture";
    if (arg_name.contains("firstprivate")) {
      attr = DataSharingAttribute::Firstprivate;
      clause = "firstprivate-name-hint";
    } else if (arg_name.contains("lastprivate")) {
      attr = DataSharingAttribute::Lastprivate;
      clause = "lastprivate-name-hint";
    } else if (arg_name.contains("private")) {
      attr = DataSharingAttribute::Private;
      clause = "private-name-hint";
    } else if (arg_name.contains("shared")) {
      attr = DataSharingAttribute::Shared;
      clause = "shared-name-hint";
    }

    if (attr != DataSharingAttribute::None) {
      addEntry(&arg, attr, clause, &func);
    }
  }
}

void DataSharingAnalysis::inferOutlinedFunctionCaptures(Function &func) {
  if (func.isDeclaration() || !func.hasName() ||
      func.getName().find(".omp_outlined") == StringRef::npos) {
    return;
  }

  for (Argument &arg : func.args()) {
    if (getAttribute(&arg) != DataSharingAttribute::None) {
      continue;
    }

    if (!arg.getType()->isPointerTy()) {
      addEntry(&arg, DataSharingAttribute::Firstprivate,
               "outlined-by-value-capture", &func);
      continue;
    }

    bool has_write = false;
    bool has_read = false;
    bool escapes_or_unknown = false;

    std::deque<const Value *> worklist;
    std::set<const Value *> visited;
    worklist.push_back(&arg);
    while (!worklist.empty()) {
      const Value *value = worklist.front();
      worklist.pop_front();
      if (!value || !visited.insert(value).second) {
        continue;
      }

      for (const User *user : value->users()) {
        if (const auto *load = dyn_cast<LoadInst>(user)) {
          if (load->getPointerOperand() == value) {
            has_read = true;
            continue;
          }
        }

        if (const auto *store = dyn_cast<StoreInst>(user)) {
          if (store->getPointerOperand() == value) {
            has_write = true;
            continue;
          }
          if (store->getValueOperand() == value) {
            escapes_or_unknown = true;
            continue;
          }
        }

        if (const auto *cb = dyn_cast<CallBase>(user)) {
          for (const Use &arg_use : cb->args()) {
            if (arg_use.get() == value) {
              escapes_or_unknown = true;
              break;
            }
          }
          continue;
        }

        if (isa<GetElementPtrInst>(user) || isa<BitCastInst>(user) ||
            isa<AddrSpaceCastInst>(user) || isa<PHINode>(user) ||
            isa<SelectInst>(user)) {
          worklist.push_back(user);
          continue;
        }

        escapes_or_unknown = true;
      }
    }

    if (has_write || escapes_or_unknown) {
      addEntry(&arg, DataSharingAttribute::Shared,
               "outlined-pointer-capture-write", &func);
    } else if (has_read) {
      addEntry(&arg, DataSharingAttribute::SharedNoModify,
               "outlined-pointer-capture-read", &func);
    } else {
      addEntry(&arg, DataSharingAttribute::Shared,
               "outlined-pointer-capture-opaque", &func);
    }
  }
}

const Value *DataSharingAnalysis::canonicalizeValue(const Value *value) const {
  if (!value) {
    return nullptr;
  }
  value = value->stripPointerCasts();
  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->isCast()) {
      return canonicalizeValue(ce->getOperand(0));
    }
  }
  return value;
}

void DataSharingAnalysis::addEntry(const Value *variable,
                                   DataSharingAttribute attribute,
                                   const std::string &clause,
                                   const Value *region) {
  const Value *canonical = canonicalizeValue(variable);
  if (!canonical) {
    return;
  }

  m_variable_attributes[canonical] = attribute;
  DataSharingEntry entry{canonical, attribute, clause};
  m_entries.push_back(entry);
  if (region) {
    m_region_entries[region].push_back(entry);
  }
}

bool DataSharingAnalysis::isPrivate(const Value *v) const {
  auto it = m_variable_attributes.find(canonicalizeValue(v));
  return it != m_variable_attributes.end() &&
         it->second == DataSharingAttribute::Private;
}

bool DataSharingAnalysis::isShared(const Value *v) const {
  auto it = m_variable_attributes.find(canonicalizeValue(v));
  return it != m_variable_attributes.end() &&
         (it->second == DataSharingAttribute::Shared ||
          it->second == DataSharingAttribute::SharedNoModify);
}

bool DataSharingAnalysis::isFirstprivate(const Value *v) const {
  auto it = m_variable_attributes.find(canonicalizeValue(v));
  return it != m_variable_attributes.end() &&
         it->second == DataSharingAttribute::Firstprivate;
}

DataSharingAttribute DataSharingAnalysis::getAttribute(const Value *v) const {
  auto it = m_variable_attributes.find(canonicalizeValue(v));
  if (it != m_variable_attributes.end())
    return it->second;
  return DataSharingAttribute::None;
}

std::vector<DataSharingEntry>
DataSharingAnalysis::getEntriesForRegion(const Value *region) const {
  auto by_region = m_region_entries.find(region);
  if (by_region != m_region_entries.end()) {
    return by_region->second;
  }
  std::vector<DataSharingEntry> result;
  for (const auto &entry : m_entries) {
    if (entry.variable == region)
      result.push_back(entry);
  }
  return result;
}
} // namespace OpenMP
